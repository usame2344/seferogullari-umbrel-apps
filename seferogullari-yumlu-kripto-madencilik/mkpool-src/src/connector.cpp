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
// File:        connector.cpp
// Description: TCP/TLS listener and connection acceptor for Stratum tiers.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "connector.hpp"
#include "metrics.hpp"

#include <spdlog/spdlog.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>   // SO_REUSEPORT

namespace mkpool {

Connector::Connector(boost::asio::io_context& accept_io,
                     IoPool& workers,
                     const CoinConfig& coin,
                     StratifierPtr strat,
                     std::shared_ptr<bitcoin::BitcoinClient> btc,
                     std::shared_ptr<RateLimiter> rl,
                     OnSession on_session,
                     std::shared_ptr<bitcoin::BitcoinClient> auxBtc,
                     std::shared_ptr<TlsReloadable> tls)
    : accept_io_(accept_io),
      workers_(workers),
      coin_(coin),
      strat_(std::move(strat)),
      btc_(std::move(btc)),
      rl_(std::move(rl)),
      on_session_(std::move(on_session)),
      auxBtc_(std::move(auxBtc)),
      tls_(std::move(tls)),
      acceptor_(accept_io) {}

void Connector::start() {
    namespace asio = boost::asio;
    using boost::asio::ip::tcp;
    boost::system::error_code ec;

    tcp::endpoint ep(asio::ip::make_address(coin_.stratumListenAddress, ec),
                     coin_.stratumListenPort);
    if (ec) {
        spdlog::error("[Connector] bad listen address '{}': {}",
                      coin_.stratumListenAddress, ec.message());
        return;
    }
    acceptor_.open(ep.protocol(), ec);
    if (ec) { spdlog::error("[Connector] open: {}", ec.message()); return; }
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    // SO_REUSEPORT lets an incoming (freshly started) instance bind this
    // same port while the outgoing instance is still draining, so a restart no
    // longer has a hard "port busy" gap and can be overlapped for near-zero
    // downtime. Works with systemd; no FD handover. Best-effort - a kernel that
    // lacks SO_REUSEPORT simply keeps the SO_REUSEADDR behaviour above.
#ifdef SO_REUSEPORT
    {
        const int on = 1;
        if (::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT,
                         &on, sizeof(on)) != 0) {
            spdlog::warn("[Connector] {} SO_REUSEPORT not set on :{} ({})",
                         coin_.name, coin_.stratumListenPort, std::strerror(errno));
        }
    }
#endif
    acceptor_.set_option(tcp::no_delay(true), ec);
    acceptor_.bind(ep, ec);
    if (ec) {
        spdlog::error("[Connector] bind {}:{} failed: {}",
                      coin_.stratumListenAddress, coin_.stratumListenPort, ec.message());
        return;
    }
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) { spdlog::error("[Connector] listen: {}", ec.message()); return; }
    running_.store(true);
    spdlog::info("[Connector] {} listening on {}:{}",
                 coin_.name, coin_.stratumListenAddress, coin_.stratumListenPort);
    do_accept();
}

void Connector::stop() {
    running_.store(false);
    boost::system::error_code ec;
    acceptor_.close(ec);
}

void Connector::do_accept() {
    if (!running_.load()) return;
    auto& worker = workers_.next();
    // Snapshot the current TLS context (if this is an encrypted listener) so a
    // mid-flight SIGHUP reload only affects subsequent accepts.
    std::shared_ptr<boost::asio::ssl::context> ctx = tls_ ? tls_->get() : nullptr;
    auto session = std::make_shared<ClientSession>(
        worker, coin_, strat_, btc_, rl_, auxBtc_, std::move(ctx));
    auto self = shared_from_this();
    acceptor_.async_accept(session->socket(),
        [self, session](const boost::system::error_code& ec) {
            self->handle_accept(ec, session);
        });
}

void Connector::handle_accept(const boost::system::error_code& ec,
                              std::shared_ptr<ClientSession> session) {
    if (!running_.load()) return;
    if (ec) {
        if (ec != boost::asio::error::operation_aborted) {
            spdlog::warn("[Connector] accept: {}", ec.message());
        }
        do_accept();
        return;
    }

    boost::system::error_code ep_ec;
    auto remote = session->socket().remote_endpoint(ep_ec);
    std::string ip = ep_ec ? std::string{"?"} : remote.address().to_string();

    if (rl_ && !rl_->allow_connection(ip)) {
        metrics::inc_connections_rejected("rate");
        spdlog::warn("[Connector] {} rejected connection from {} (rate-limiter: banned or over per-IP limit)",
                     coin_.name, ip);
        boost::system::error_code cec;
        session->socket().close(cec);
        do_accept();
        return;
    }
    try {
        session->setDisconnectHandler([](std::shared_ptr<ClientSession>) {});
        if (on_session_) on_session_(session);
        spdlog::info("[Connector] accepted connection from {}", ip);
        session->start();
    } catch (const std::exception& e) {
        spdlog::error("[Connector] session start failed: {}", e.what());
    }
    do_accept();
}

} // namespace mkpool
