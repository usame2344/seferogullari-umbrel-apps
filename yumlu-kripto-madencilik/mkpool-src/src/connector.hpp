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
// File:        connector.hpp
// Description: Stratum listener/acceptor interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <string>

#include "io_pool.hpp"
#include "client_session.hpp"
#include "tls_context.hpp"

namespace mkpool {

class Connector : public std::enable_shared_from_this<Connector> {
public:
    using OnSession = std::function<void(std::shared_ptr<ClientSession>)>;

    // `tls` non-null makes this an encrypted (TLS) listener. The holder is read
    // at accept time so a SIGHUP certificate reload takes effect for new
    // connections without restarting the listener.
    Connector(boost::asio::io_context& accept_io,
              IoPool& workers,
              const CoinConfig& coin,
              StratifierPtr strat,
              std::shared_ptr<bitcoin::BitcoinClient> btc,
              std::shared_ptr<RateLimiter> rl,
              OnSession on_session,
              std::shared_ptr<bitcoin::BitcoinClient> auxBtc = nullptr,
              std::shared_ptr<TlsReloadable> tls = nullptr);

    void start();
    void stop();

private:
    void do_accept();
    void handle_accept(const boost::system::error_code& ec,
                       std::shared_ptr<ClientSession> session);

    boost::asio::io_context& accept_io_;
    IoPool& workers_;
    const CoinConfig& coin_;
    StratifierPtr strat_;
    std::shared_ptr<bitcoin::BitcoinClient> btc_;
    std::shared_ptr<RateLimiter> rl_;
    OnSession on_session_;
    std::shared_ptr<bitcoin::BitcoinClient> auxBtc_{nullptr};
    std::shared_ptr<TlsReloadable> tls_{nullptr};

    boost::asio::ip::tcp::acceptor acceptor_;
    std::atomic<bool> running_{false};
};

} // namespace mkpool
