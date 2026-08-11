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
// File:        cluster_ingest.hpp
// Description: Origin-side cluster trunk acceptor: demux + loopback bridge.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "relay_session.hpp"
#include "upstream_client.hpp"   // reuse UpstreamConn as the loopback transport

namespace mkpool {

class ClusterBlockRelay;

// One accepted cluster trunk on the ORIGIN (a pool-role process). It reads frames
// from an edge (passthrough / node), and for every downstream miner the edge
// announces it opens a plain loopback Stratum connection to this coin's own local
// stratum port and shuttles bytes both ways. The origin therefore serves each
// clustered miner exactly as if it had connected directly - full vardiff,
// accounting, coinbase and block submission - with ZERO changes to any solo code.
//
// Only ever constructed when a coin has cluster.ingestPort set, so it is inert in
// a normal pool deployment.
class ClusterIngestSession final : public RelaySession {
public:
    ClusterIngestSession(boost::asio::io_context& io,
                         std::shared_ptr<RateLimiter> rl,
                         std::shared_ptr<boost::asio::ssl::context> tls_ctx,
                         std::string forward_host,
                         std::uint16_t forward_port,
                         std::string coin,
                         std::string token,
                         std::shared_ptr<ClusterBlockRelay> block_relay = nullptr);

    // Called by ClusterBlockRelay (node role only) to push a found block down
    // this trunk. Strand-safe.
    void deliver_block(const std::string& hex, const std::string& hash, std::int64_t height);

protected:
    [[nodiscard]] const char* role_label() const noexcept override { return "cluster-ingest"; }
    void on_line(std::string_view line) override;   // one protocol frame
    void on_closed() override;

private:
    using json = nlohmann::json;

    void handle_hello(const json& j);
    void open_client(std::uint64_t cid, std::string ip);
    void client_data(std::uint64_t cid, std::string line);
    void close_client(std::uint64_t cid, bool tell_edge);
    void loopback_connected(std::uint64_t cid);   // flush pre-connect buffer

    // A downstream miner bridged to a loopback stratum connection. `conn` runs on
    // its own strand; all access to this map is on the ingest session strand.
    struct Client {
        std::shared_ptr<UpstreamConn> conn;
        bool                          connected{false};
        std::deque<std::string>       pending;  // miner lines awaiting connect
    };

    std::string   forward_host_;
    std::uint16_t forward_port_;
    std::string   coin_;
    std::string   token_;
    std::shared_ptr<ClusterBlockRelay> block_relay_;  // node block fan-out (may be null)
    bool          hello_ok_{false};
    bool          is_node_{false};
    std::uint64_t clients_seen_{0};

    std::unordered_map<std::uint64_t, Client> clients_;  // cid -> bridged miner
};

} // namespace mkpool
