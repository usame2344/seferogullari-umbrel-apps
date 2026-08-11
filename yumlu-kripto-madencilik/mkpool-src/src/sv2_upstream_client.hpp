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
// File:        sv2_upstream_client.hpp
// Description: Outbound Stratum V2 client (V1 downstream -> SV2 upstream translation).
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

#include "config.hpp"
#include "sv2_messages.hpp"
#include "upstream_client.hpp"   // UpstreamSource, ProxyNotify, ProxySlot, ProxyWorkSink

namespace mkpool {

// Outbound Stratum V2 client that presents itself to downstream V1 miners as an
// ordinary V1 upstream (implements UpstreamSource). It opens an SV2 EXTENDED
// mining channel to an SV2 pool, then translates: NewExtendedMiningJob +
// SetNewPrevHash -> a V1 mining.notify (ProxyNotify), and a downstream V1
// mining.submit -> SubmitSharesExtended. The extended channel's extranonce space
// is subdivided across downstream miners exactly like the V1 UpstreamClient
// subdivides a V1 extranonce. This lets a plain Stratum V1 rig (which is most
// hardware) mine on an SV2-only pool. Single upstream link (no multi-link
// failover yet); plaintext SV2 transport (Noise-client upstream is a follow-up).
// Only constructed when global.role == Proxy and cluster/upstream selects SV2.
class Sv2UpstreamClient : public UpstreamSource,
                          public std::enable_shared_from_this<Sv2UpstreamClient> {
public:
    Sv2UpstreamClient(boost::asio::io_context& io, CoinConfig coin);

    // ---- UpstreamSource ----
    void start() override;
    void stop() override;
    [[nodiscard]] ProxySlot attach(const std::weak_ptr<ProxyWorkSink>& sink) override;
    void detach(int link_id, std::uint32_t slot_id) override;
    void register_waiter(const std::weak_ptr<ProxyWorkSink>& sink) override;
    [[nodiscard]] ProxyNotifyPtr current_job(int link_id) const override;
    [[nodiscard]] double current_diff(int link_id) const override;
    [[nodiscard]] bool any_ready() const override;
    void submit(int link_id, const UpstreamSubmit& s, SubmitCallback cb = {}) override;
    [[nodiscard]] UpstreamStatus status() const override;

private:
    using Strand = boost::asio::strand<boost::asio::any_io_executor>;

    // transport (plaintext SV2 over TCP), all on strand_
    void connect();
    void schedule_reconnect();
    void do_read();
    void on_read(const boost::system::error_code& ec, std::size_t n);
    void consume_frames();
    void send_sv2(std::uint8_t type, const sv2::Writer& payload);
    void do_write();
    void teardown(const std::string& reason);

    // SV2 protocol handlers (on strand_)
    void handle_frame(const sv2::Header& h, sv2::Reader& r);
    void on_channel_open(const sv2::OpenExtendedMiningChannelSuccess& ok);
    void on_extended_job(const sv2::NewExtendedMiningJob& j);
    void on_set_prev_hash(const sv2::SetNewPrevHash& ph);
    void on_set_target(const std::array<std::uint8_t, 32>& target);
    void on_submit_result(std::uint32_t seq, bool accepted, const std::string& err);

    // Rebuild the current ProxyNotify from the latest extended job + prev-hash and
    // broadcast it to all subscribers. Returns null until both are present.
    void rebuild_and_broadcast(bool clean);
    [[nodiscard]] std::vector<std::shared_ptr<ProxyWorkSink>> live_sinks() const;
    void notify_waiters();

    boost::asio::io_context& io_;
    Strand strand_;
    CoinConfig coin_;

    // one upstream endpoint (first configured)
    std::string host_, port_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer reconnect_timer_;
    int reconnect_backoff_{0};
    std::atomic<bool> running_{false};

    // read/write buffers
    std::array<std::uint8_t, 8192> read_buf_{};
    std::vector<std::uint8_t> in_;
    bool have_header_{false};
    sv2::Header cur_header_{};
    std::deque<std::vector<std::uint8_t>> wq_;
    bool writing_{false};

    // SV2 channel state (guarded by mu_ where read cross-thread)
    mutable std::mutex mu_;
    bool connected_{false}, setup_ok_{false}, channel_ok_{false};
    std::uint32_t channel_id_{0};
    std::vector<std::uint8_t> extranonce_prefix_;   // fixed channel prefix
    std::uint16_t extranonce_size_{0};               // proxy-controlled bytes
    std::uint8_t  nonce_bytes_{2};                    // per-downstream slot width
    double diff_{1.0};

    // latest work pieces
    struct ExtJob {
        std::uint32_t sv2_job_id{0};
        std::uint32_t version{0};
        bool version_rolling_allowed{true};
        std::vector<std::array<std::uint8_t, 32>> merkle_path;
        std::vector<std::uint8_t> cb_prefix, cb_suffix;
        bool have{false};
    } job_;
    struct PrevHash {
        std::uint32_t sv2_job_id{0};
        std::array<std::uint8_t, 32> prev_hash{};
        std::uint32_t min_ntime{0}, nbits{0};
        bool have{false};
    } prev_;

    ProxyNotifyPtr cur_notify_;                       // guarded by mu_
    std::uint64_t notify_seq_{0};
    // Map the V1 job id string we hand downstream -> the SV2 job id to submit with.
    std::unordered_map<std::string, std::uint32_t> jobid_map_;
    std::deque<std::string> jobid_order_;

    // slot allocator over the extranonce space
    std::uint32_t next_slot_{0};
    std::unordered_map<std::uint32_t, std::weak_ptr<ProxyWorkSink>> subscribers_; // slot -> sink
    std::vector<std::weak_ptr<ProxyWorkSink>> waiters_;

    // submit correlation
    std::uint32_t next_seq_{1};
    std::unordered_map<std::uint32_t, SubmitCallback> pending_;

    // stats
    std::atomic<std::uint64_t> stat_jobs_{0}, stat_forwarded_{0}, stat_accepted_{0},
                               stat_rejected_{0}, stat_reconnects_{0};
};

} // namespace mkpool
