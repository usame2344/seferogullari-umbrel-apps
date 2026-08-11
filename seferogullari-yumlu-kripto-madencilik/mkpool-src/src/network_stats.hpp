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
// File:        network_stats.hpp
// Description: Network statistics interface.
// Created:     2026-06-02
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "bitcoin_client.hpp"
#include "config.hpp"
#include "zmq_client.hpp"

namespace mkpool {

class NetworkStatsTracker
    : public std::enable_shared_from_this<NetworkStatsTracker> {
public:
    NetworkStatsTracker(boost::asio::io_context& io,
                        std::shared_ptr<bitcoin::BitcoinClient> btc,
                        CoinConfig coin,
                        std::chrono::seconds heartbeat = std::chrono::seconds(30));

    void start();
    void stop();

    // Trigger a stats refresh from an external new-block source (the Generator's
    // single hashblock subscription) instead of opening our own ZMQ socket.
    void notifyNewBlock();
    // When true, start() does NOT open its own ZMQ subscription; refreshes come
    // from notifyNewBlock() + the heartbeat. Must be set before start().
    void setExternalBlockFeed(bool v) { external_block_feed_ = v; }

private:
    void schedule_heartbeat();
    void refresh();
    void start_zmq();
    void commit(const nlohmann::json& state);

    boost::asio::io_context& io_;
    std::shared_ptr<bitcoin::BitcoinClient> btc_;
    CoinConfig coin_;
    std::chrono::seconds heartbeat_;
    boost::asio::steady_timer timer_;
    std::shared_ptr<ZMQClient> zmq_;
    std::atomic<bool> running_{false};
    std::atomic<bool> in_flight_{false};
    bool external_block_feed_{false};
};

} // namespace mkpool
