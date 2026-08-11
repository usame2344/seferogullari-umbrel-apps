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
// File:        cluster_block_relay.hpp
// Description: Origin-side fan-out of found blocks to connected node trunks.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/asio.hpp>

namespace mkpool {

class ZMQClient;
class ClusterIngestSession;

// Subscribes to the origin node's ZMQ rawblock stream and pushes every accepted
// block, verbatim, to the node-role trunks currently connected to this coin's
// cluster ingest. A node then submits the block to its OWN local bitcoind for
// geographically redundant / faster propagation. Purely additive: it reads the
// node's existing rawblock feed and never touches the solo block path. Passthrough
// trunks do not register here, so they are unaffected.
class ClusterBlockRelay : public std::enable_shared_from_this<ClusterBlockRelay> {
public:
    ClusterBlockRelay(boost::asio::io_context& io, std::string zmq_endpoint, std::string coin);

    void start();
    void stop();

    // A node trunk registers here when its hello names role "node". Registration
    // is by weak_ptr, so a closed session simply drops out on the next fan-out.
    void register_node(const std::weak_ptr<ClusterIngestSession>& s);

private:
    void on_rawblock(const std::string& raw);   // raw serialized block bytes

    boost::asio::io_context& io_;
    std::string zmq_endpoint_;
    std::string coin_;
    std::shared_ptr<ZMQClient> zmq_;

    mutable std::mutex mu_;
    std::vector<std::weak_ptr<ClusterIngestSession>> nodes_;
    std::uint64_t blocks_relayed_{0};
};

using ClusterBlockRelayPtr = std::shared_ptr<ClusterBlockRelay>;

} // namespace mkpool
