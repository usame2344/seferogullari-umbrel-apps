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
// File:        cluster_trunk.hpp
// Description: Edge-side (passthrough/node) cluster trunk + downstream session.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

#include "config.hpp"
#include "relay_session.hpp"

namespace mkpool {

class UpstreamConn;          // reused as the outbound trunk transport
namespace bitcoin { class BitcoinClient; }

// A downstream miner connected to a passthrough / node. It is an ordinary Stratum
// V1 (+TLS) endpoint whose bytes are relayed verbatim, in both directions, over
// the shared cluster trunk to the origin. It does NO stratum parsing itself: the
// origin owns subscribe/authorize/notify/submit and all accounting. Only ever
// constructed in passthrough / node role.
class ClusterEdgeSession final : public RelaySession {
public:
    ClusterEdgeSession(boost::asio::io_context& io,
                       std::shared_ptr<RateLimiter> rl,
                       std::shared_ptr<class ClusterTrunk> trunk,
                       std::shared_ptr<boost::asio::ssl::context> tls_ctx);

    // origin -> miner: deliver one stratum line (no trailing newline). Strand-safe.
    void deliver(std::string_view line);

protected:
    [[nodiscard]] const char* role_label() const noexcept override { return "cluster-edge"; }
    void on_open() override;
    void on_line(std::string_view line) override;  // miner -> origin
    void on_closed() override;

private:
    std::shared_ptr<ClusterTrunk> trunk_;
    std::uint64_t cid_{0};
};

// Owns the outbound trunk connection(s) to an origin for one proxied coin and
// multiplexes every downstream ClusterEdgeSession over it. In node role it also
// applies origin-streamed blocks to a local bitcoind. Constructed only when
// global.role is Passthrough or Node and cluster.uplink_on().
class ClusterTrunk : public std::enable_shared_from_this<ClusterTrunk> {
public:
    ClusterTrunk(boost::asio::io_context& io, CoinConfig coin, bool node_role);

    void start();
    void stop();

    // Downstream session lifecycle. attach() reserves a cid and announces the
    // miner upstream (buffered until the trunk handshake completes); returns the
    // cid (always > 0). client_line() relays one miner line; detach() closes it.
    std::uint64_t attach(const std::weak_ptr<ClusterEdgeSession>& s, const std::string& ip);
    void client_line(std::uint64_t cid, std::string line);
    void detach(std::uint64_t cid);

    [[nodiscard]] bool ready() const noexcept { return welcomed_.load(); }

private:
    using Strand = boost::asio::strand<boost::asio::any_io_executor>;

    void connect();
    void schedule_reconnect();
    void on_open();
    void on_close(const std::string& reason);
    void on_frame(std::string_view line);
    void send_frame(std::string frame);          // buffer-until-welcome then write
    void drop_all_sessions(const char* reason);
    void submit_block(const std::string& hex, const std::string& hash, std::int64_t height);

    boost::asio::io_context& io_;
    Strand strand_;
    CoinConfig coin_;
    bool node_role_{false};

    std::shared_ptr<UpstreamConn> conn_;         // strand-only
    boost::asio::steady_timer reconnect_timer_;
    int reconnect_backoff_{0};                   // strand-only
    std::atomic<bool> welcomed_{false};
    std::atomic<bool> running_{false};
    std::deque<std::string> pending_out_;        // frames awaiting welcome (strand-only)

    // Node-only: local node RPC for submitblock (null in passthrough role).
    std::shared_ptr<bitcoin::BitcoinClient> local_node_;

    std::atomic<std::uint64_t> next_cid_{1};
    mutable std::mutex mu_;
    std::unordered_map<std::uint64_t, std::weak_ptr<ClusterEdgeSession>> sessions_;  // cid -> session

    // Stats (atomic; surfaced via the control socket).
    std::atomic<std::uint64_t> stat_opened_{0}, stat_closed_{0}, stat_reconnects_{0},
                               stat_blocks_{0};
};

using ClusterTrunkPtr = std::shared_ptr<ClusterTrunk>;

} // namespace mkpool
