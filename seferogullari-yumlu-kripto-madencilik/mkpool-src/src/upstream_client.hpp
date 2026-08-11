// SPDX-License-Identifier: GPL-3.0
// Copyright (c) 2025-2026 Mecanik1337 <contact@mecanik.dev>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// File:        upstream_client.hpp
// Description: Outbound Stratum V1 client + multi-link relay state for the proxy role.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <nlohmann/json.hpp>

#include "any_stream.hpp"
#include "config.hpp"

namespace mkpool {

// Immutable, pre-rendered snapshot of one upstream link's current job. In a
// Stratum proxy the coinbase (cb1/cb2), merkle branch and header fields come
// from the upstream pool and are identical for every downstream miner on that
// link - each only differs by its extranonce (set at subscribe time) - so a
// single rendered mining.notify line is broadcast to all of them.
struct ProxyNotify {
    std::string notify_line;                 // full mining.notify JSON + '\n', shared by all downstream on the link
    std::string job_id;                      // upstream job id (relayed verbatim so submits map back)
    std::string prevhash_stratum;            // as received from upstream
    std::string coinbase1;
    std::string coinbase2;
    std::vector<std::string> merkle_branch;  // stratum-order hex
    std::uint32_t version{0};                // decoded (big-endian value)
    std::string  bits;                       // stratum hex (compact target)
    std::string  ntime;                      // stratum hex (upstream-suggested time)
    std::uint32_t mintime{0};                // decoded ntime; earliest time a miner may use
    bool clean{false};
    std::array<std::uint8_t, 32> net_target_be{}; // decoded from bits, for block detection
    std::uint64_t seq{0};                    // monotonically increasing per-link job sequence
};
using ProxyNotifyPtr = std::shared_ptr<const ProxyNotify>;

// A per-downstream extranonce slice carved out of one link's extranonce space.
struct ProxySlot {
    bool          ok{false};
    int           link_id{-1};// which upstream link owns this slot
    std::uint32_t slot_id{0};
    std::string   slot_hex;   // the reserved slot bytes (link.nonceBytes wide)
    std::string   en1_hex;    // downstream extranonce1 = link_en1 || slot_hex
    std::size_t   en2_size{0};// downstream extranonce2 size = link_en2_size - nonceBytes
};

// A downstream share ready to be forwarded to its link. en2_full_hex is the full
// upstream extranonce2 = slot_hex || downstream_en2.
struct UpstreamSubmit {
    std::string job_id;
    std::string en2_full_hex;
    std::string ntime_hex;
    std::string nonce_hex;
    std::optional<std::uint32_t> version_bits;  // present only if version-rolling granted upstream
};

// Interface a downstream proxy session implements to receive upstream events.
// All methods must be cheap and thread-safe (implementations post to their own
// strand); UpstreamClient calls them from its own strand.
class ProxyWorkSink {
public:
    virtual ~ProxyWorkSink() = default;
    virtual void on_upstream_job(ProxyNotifyPtr job) = 0;
    virtual void on_upstream_diff(double diff) = 0;
    // Live extranonce change on the SAME link (reconnect / set_extranonce): the
    // session's recomputed en1/en2_size are passed so it can push set_extranonce.
    virtual void on_upstream_extranonce(std::string en1_hex, std::size_t en2_size) = 0;
    virtual void on_upstream_state(bool up) = 0;
    // Migrated to a DIFFERENT link (hot-standby failover / active-active
    // rebalance): the session adopts the new slot binding, difficulty and job.
    virtual void on_rebind(ProxySlot slot, ProxyNotifyPtr job, double diff) = 0;
};

// Per-link status for the control socket / metrics.
struct LinkStatus {
    int          index{-1};
    std::string  endpoint;
    bool         connected{false};
    bool         authorized{false};
    bool         primary{false};
    double       difficulty{0.0};
    std::string  extranonce1;
    std::size_t  en2_size{0};
    std::uint64_t jobs{0};
    std::uint64_t shares_forwarded{0};
    std::uint64_t shares_accepted{0};
    std::uint64_t shares_rejected{0};
    std::uint64_t reconnects{0};
    std::size_t  sessions{0};
};
struct UpstreamStatus {
    std::string mode;
    int         primary{-1};
    std::size_t total_sessions{0};
    std::vector<LinkStatus> links;
};

// Transport-only outbound connection: TCP (or TLS) connect, then a Stratum V1
// line reader/writer. Protocol logic lives in UpstreamClient, which owns the
// connection and feeds it lines. Recreated per connection attempt so its
// lifetime is clean (handlers hold shared_ptr self, like the sessions).
class UpstreamConn : public std::enable_shared_from_this<UpstreamConn> {
public:
    using LineHandler  = std::function<void(std::string_view line)>;
    using OpenHandler  = std::function<void()>;
    using CloseHandler = std::function<void(const std::string& reason)>;

    UpstreamConn(boost::asio::io_context& io,
                 std::string host, std::string port,
                 bool tls,
                 std::shared_ptr<boost::asio::ssl::context> tls_ctx,
                 LineHandler on_line, OpenHandler on_open, CloseHandler on_close);

    void start();               // resolve + connect + (TLS) handshake + read
    void send(std::string line);// enqueue a line (caller includes trailing '\n')
    void close(const std::string& reason);
    [[nodiscard]] bool closed() const noexcept { return closed_.load(); }

private:
    using Strand = boost::asio::strand<boost::asio::any_io_executor>;
    void do_read();
    void on_read(const boost::system::error_code& ec, std::size_t n);
    void do_write();

    boost::asio::io_context& io_;
    Strand strand_;
    boost::asio::ip::tcp::resolver resolver_;
    std::string host_, port_;
    std::shared_ptr<boost::asio::ssl::context> tls_ctx_;
    bool is_tls_{false};
    AnyStream socket_;

    LineHandler  on_line_;
    OpenHandler  on_open_;
    CloseHandler on_close_;

    std::array<char, 8192> read_buf_{};
    std::string buffer_;
    std::deque<std::string> wq_;
    bool writing_{false};
    std::atomic<bool> closed_{false};
};

// Abstract work source a downstream proxy/relay session pulls from. Implemented
// by UpstreamClient (V1 upstream) and Sv2UpstreamClient (SV2 upstream), so the
// same downstream session code serves miners regardless of the upstream protocol
// (this is what makes the proxy bidirectional: V1<->SV2 in either direction).
class UpstreamSource {
public:
    using SubmitCallback = std::function<void(bool accepted, const std::string& err)>;
    virtual ~UpstreamSource() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual ProxySlot attach(const std::weak_ptr<ProxyWorkSink>& sink) = 0;
    virtual void detach(int link_id, std::uint32_t slot_id) = 0;
    virtual void register_waiter(const std::weak_ptr<ProxyWorkSink>& sink) = 0;
    [[nodiscard]] virtual ProxyNotifyPtr current_job(int link_id) const = 0;
    [[nodiscard]] virtual double current_diff(int link_id) const = 0;
    [[nodiscard]] virtual bool any_ready() const = 0;
    virtual void submit(int link_id, const UpstreamSubmit& s, SubmitCallback cb = {}) = 0;
    [[nodiscard]] virtual UpstreamStatus status() const = 0;
};

// Outbound Stratum client + multi-link relay state for a single proxied coin.
// Every configured endpoint becomes a "link" that connects and stays warm, so a
// standby is ready for instant (sub-second) failover. Downstream sessions are
// assigned to a link per the configured mode (failover / active-active) and
// migrated between links on failure/rebalance. Only constructed when
// global.role == Proxy, so it is inert otherwise.
class UpstreamClient : public UpstreamSource,
                       public std::enable_shared_from_this<UpstreamClient> {
public:
    using SubmitCallback = UpstreamSource::SubmitCallback;

    UpstreamClient(boost::asio::io_context& io, CoinConfig coin);

    void start();
    void stop();

    // Downstream session lifecycle. attach() picks a link per the mode, reserves
    // a slot on it and registers the sink; the returned slot is empty (ok=false)
    // if no link is ready (the caller should register_waiter() and wait).
    ProxySlot attach(const std::weak_ptr<ProxyWorkSink>& sink);
    void detach(int link_id, std::uint32_t slot_id);
    // Register a session that could not attach yet; it is pinged (on_upstream_state
    // true) whenever any link becomes ready so it can retry.
    void register_waiter(const std::weak_ptr<ProxyWorkSink>& sink);

    // Current work/diff for the session's link (post-attach). Null / 0 if not ready.
    [[nodiscard]] ProxyNotifyPtr current_job(int link_id) const;
    [[nodiscard]] double current_diff(int link_id) const;
    [[nodiscard]] bool any_ready() const;            // at least one link ready

    // Forward a downstream share to its link. cb (optional) gets the verdict.
    void submit(int link_id, const UpstreamSubmit& s, SubmitCallback cb = {});

    [[nodiscard]] UpstreamStatus status() const;

private:
    // A PendingSubmit correlates a forwarded share to its callback.
    struct PendingSubmit { SubmitCallback cb; std::chrono::steady_clock::time_point ts; };
    struct Subscriber { std::weak_ptr<ProxyWorkSink> sink; std::string slot_hex; };

    // One managed upstream connection + its handshake and slot-allocator state.
    struct Link {
        explicit Link(boost::asio::io_context& io) : reconnect_timer(io) {}
        UpstreamEndpoint ep;
        int index{-1};

        std::shared_ptr<UpstreamConn> conn;
        boost::asio::steady_timer reconnect_timer;
        std::uint64_t conn_gen{0};      // strand-only: drops stale callbacks
        int reconnect_backoff{0};       // strand-only
        std::chrono::steady_clock::time_point last_rx{}; // strand-only

        std::uint64_t next_id{1};       // strand-only JSON-RPC id source
        std::uint64_t id_configure{0}, id_subscribe{0}, id_authorize{0};
        std::unordered_map<std::uint64_t, PendingSubmit> pending; // strand-only

        // Handshake / relay state (guarded by the client mu_).
        bool connected{false}, authorized{false}, subscribed{false};
        std::string up_en1;
        std::size_t up_en2_size{0};
        std::uint32_t up_version_mask{0};
        double up_diff{1.0};
        ProxyNotifyPtr cur_job;
        std::uint64_t job_seq{0};

        // Slot allocator (guarded by the client mu_).
        std::uint32_t slot_span{0};
        std::uint8_t  nonce_bytes_eff{0};
        std::uint32_t next_slot{0};
        std::unordered_map<std::uint32_t, Subscriber> subscribers; // slot_id -> subscriber

        // Counters (atomic).
        std::atomic<std::uint64_t> stat_jobs{0}, stat_forwarded{0}, stat_accepted{0},
                                   stat_rejected{0}, stat_reconnects{0};
    };

    // Connection lifecycle (all on strand_).
    void connect_link(int i);
    void schedule_reconnect(int i);
    void arm_idle_watchdog();
    void on_conn_open(int i);
    void on_conn_close(int i, const std::string& reason);
    void on_line(int i, std::string_view line);
    void send_configure(int i);
    void send_subscribe(int i);
    void send_authorize(int i);
    void handle_result(int i, const nlohmann::json& msg);
    void handle_method(int i, const std::string& method, const nlohmann::json& msg);
    void set_upstream_extranonce(int i, std::string en1, std::size_t en2_size, bool reconnect);
    void set_upstream_diff(int i, double diff);
    void build_and_broadcast_notify(int i, const nlohmann::json& params);

    // Assignment + migration helpers. *_locked assume mu_ is held.
    [[nodiscard]] bool link_ready_locked(const Link& l) const;
    [[nodiscard]] int  pick_link_locked() const;              // choose a link per mode
    [[nodiscard]] ProxySlot allocate_on_locked(Link& l, const std::weak_ptr<ProxyWorkSink>& sink);
    void migrate_link(int i);                                 // strand_: move i's sessions off it
    void notify_waiters();                                    // ping pending sessions to retry
    std::vector<std::shared_ptr<ProxyWorkSink>> link_sinks(int i) const;

    boost::asio::io_context& io_;
    using Strand = boost::asio::strand<boost::asio::any_io_executor>;
    Strand strand_;
    CoinConfig coin_;
    bool active_active_{false};

    std::vector<std::unique_ptr<Link>> links_;
    boost::asio::steady_timer idle_timer_;
    std::atomic<bool> running_{false};

    mutable std::mutex mu_;
    int primary_{-1};   // sticky serving link for failover mode (guarded by mu_)
    std::vector<std::weak_ptr<ProxyWorkSink>> waiters_; // sessions awaiting a ready link
};

using UpstreamClientPtr = std::shared_ptr<UpstreamClient>;

} // namespace mkpool
