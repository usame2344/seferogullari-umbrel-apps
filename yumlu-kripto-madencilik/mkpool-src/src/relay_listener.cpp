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
// File:        relay_listener.cpp
// Description: Generic TCP/TLS acceptor for the scale-out relay session types.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "relay_listener.hpp"
#include "metrics.hpp"

#include <spdlog/spdlog.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>   // SO_REUSEPORT

namespace mkpool {

RelayListener::RelayListener(boost::asio::io_context& accept_io,
                             IoPool& workers,
                             std::string bind_addr,
                             std::uint16_t port,
                             std::shared_ptr<RateLimiter> rl,
                             Factory factory,
                             std::shared_ptr<TlsReloadable> tls,
                             std::string label)
    : accept_io_(accept_io),
      workers_(workers),
      bind_addr_(std::move(bind_addr)),
      port_(port),
      rl_(std::move(rl)),
      factory_(std::move(factory)),
      tls_(std::move(tls)),
      label_(std::move(label)),
      acceptor_(accept_io) {}

void RelayListener::start() {
    namespace asio = boost::asio;
    using boost::asio::ip::tcp;
    boost::system::error_code ec;

    tcp::endpoint ep(asio::ip::make_address(bind_addr_, ec), port_);
    if (ec) {
        spdlog::error("[{}] bad listen address '{}': {}", label_, bind_addr_, ec.message());
        return;
    }
    acceptor_.open(ep.protocol(), ec);
    if (ec) { spdlog::error("[{}] open: {}", label_, ec.message()); return; }
    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    // SO_REUSEPORT for overlapped, near-zero-downtime restarts (same rationale as
    // the solo Connector). Best-effort.
#ifdef SO_REUSEPORT
    {
        const int on = 1;
        if (::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT,
                         &on, sizeof(on)) != 0) {
            spdlog::warn("[{}] SO_REUSEPORT not set on :{} ({})",
                         label_, port_, std::strerror(errno));
        }
    }
#endif
    acceptor_.set_option(tcp::no_delay(true), ec);
    acceptor_.bind(ep, ec);
    if (ec) {
        spdlog::error("[{}] bind {}:{} failed: {}", label_, bind_addr_, port_, ec.message());
        return;
    }
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) { spdlog::error("[{}] listen: {}", label_, ec.message()); return; }
    running_.store(true);
    spdlog::info("[{}] listening on {}:{}{}", label_, bind_addr_, port_,
                 tls_ ? " (tls)" : "");
    do_accept();
}

void RelayListener::stop() {
    running_.store(false);
    boost::system::error_code ec;
    acceptor_.close(ec);
}

void RelayListener::do_accept() {
    if (!running_.load()) return;
    auto& worker = workers_.next();
    std::shared_ptr<boost::asio::ssl::context> ctx = tls_ ? tls_->get() : nullptr;
    std::shared_ptr<RelaySession> session;
    try {
        session = factory_(worker, std::move(ctx));
    } catch (const std::exception& e) {
        spdlog::error("[{}] session factory failed: {}", label_, e.what());
    }
    if (!session) {
        // Never spin: re-arm on the next loop tick so a transient factory failure
        // cannot busy-loop the accept thread.
        auto self = shared_from_this();
        boost::asio::post(accept_io_, [self] { self->do_accept(); });
        return;
    }
    auto self = shared_from_this();
    acceptor_.async_accept(session->socket(),
        [self, session](const boost::system::error_code& ec) {
            self->handle_accept(ec, session);
        });
}

void RelayListener::handle_accept(const boost::system::error_code& ec,
                                  std::shared_ptr<RelaySession> session) {
    if (!running_.load()) return;
    if (ec) {
        if (ec != boost::asio::error::operation_aborted)
            spdlog::warn("[{}] accept: {}", label_, ec.message());
        do_accept();
        return;
    }

    boost::system::error_code ep_ec;
    auto remote = session->socket().remote_endpoint(ep_ec);
    std::string ip = ep_ec ? std::string{"?"} : remote.address().to_string();

    if (rl_ && !rl_->allow_connection(ip)) {
        metrics::inc_connections_rejected("rate");
        spdlog::warn("[{}] rejected connection from {} (rate-limiter)", label_, ip);
        boost::system::error_code cec;
        session->socket().close(cec);
        do_accept();
        return;
    }
    try {
        session->start();
    } catch (const std::exception& e) {
        spdlog::error("[{}] session start failed: {}", label_, e.what());
    }
    do_accept();
}

} // namespace mkpool
