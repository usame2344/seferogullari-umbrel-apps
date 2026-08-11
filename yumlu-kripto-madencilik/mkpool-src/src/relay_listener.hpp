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
// File:        relay_listener.hpp
// Description: Generic TCP/TLS acceptor for the scale-out relay session types.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <string>

#include "io_pool.hpp"
#include "rate_limiter.hpp"
#include "relay_session.hpp"
#include "tls_context.hpp"

namespace mkpool {

// Accepts downstream miner connections for a relay role and hands each to a
// caller-supplied factory. This is a standalone sibling of Connector: it shares
// none of Connector's ClientSession/Stratifier wiring, so the solo path is
// entirely unaffected. The factory receives the worker io_context the session
// should run on and the (optional) TLS context snapshot for this accept.
class RelayListener : public std::enable_shared_from_this<RelayListener> {
public:
    using Factory = std::function<std::shared_ptr<RelaySession>(
        boost::asio::io_context& worker,
        std::shared_ptr<boost::asio::ssl::context> tls_ctx)>;

    RelayListener(boost::asio::io_context& accept_io,
                  IoPool& workers,
                  std::string bind_addr,
                  std::uint16_t port,
                  std::shared_ptr<RateLimiter> rl,
                  Factory factory,
                  std::shared_ptr<TlsReloadable> tls = nullptr,
                  std::string label = "relay");

    void start();
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void do_accept();
    void handle_accept(const boost::system::error_code& ec,
                       std::shared_ptr<RelaySession> session);

    boost::asio::io_context& accept_io_;
    IoPool& workers_;
    std::string bind_addr_;
    std::uint16_t port_;
    std::shared_ptr<RateLimiter> rl_;
    Factory factory_;
    std::shared_ptr<TlsReloadable> tls_;
    std::string label_;

    boost::asio::ip::tcp::acceptor acceptor_;
    std::atomic<bool> running_{false};
};

} // namespace mkpool
