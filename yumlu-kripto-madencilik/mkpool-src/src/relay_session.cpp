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
// File:        relay_session.cpp
// Description: Shared downstream transport base for the scale-out relay roles.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "relay_session.hpp"

#include "metrics.hpp"
#include <spdlog/spdlog.h>

namespace mkpool {

RelaySession::RelaySession(boost::asio::io_context& io,
                           std::shared_ptr<RateLimiter> rl,
                           std::shared_ptr<boost::asio::ssl::context> tls_ctx)
    : io_(io),
      strand_(boost::asio::make_strand(io.get_executor())),
      tls_ctx_(std::move(tls_ctx)),
      is_tls_(tls_ctx_ != nullptr),
      socket_(io, tls_ctx_.get()),
      writes_(socket_),
      rl_(std::move(rl)) {
    writes_.set_drop_handler([this] { close("write watermark"); });
}

void RelaySession::start() {
    // Same descriptor-safety discipline as the solo path: keep the session alive
    // for the duration of each chained write, and serialise the write chain on
    // this session's strand so it can never race the strand-bound socket close.
    writes_.set_owner(std::weak_ptr<void>(shared_from_this()));
    writes_.set_executor(strand_);

    boost::system::error_code ec;
    socket_.lowest_layer().set_option(boost::asio::ip::tcp::no_delay(true), ec);
    auto ep = socket_.lowest_layer().remote_endpoint(ec);
    client_ip_ = ec ? std::string{"?"} : ep.address().to_string();
    metrics::inc_connections_accepted();

    auto self = shared_from_this();
    if (is_tls_) {
        socket_.async_handshake(
            boost::asio::ssl::stream_base::server,
            boost::asio::bind_executor(strand_,
                [self](const boost::system::error_code& hec) {
                    if (hec) { self->close("TLS handshake failed"); return; }
                    spdlog::debug("[{} {}] TLS handshake complete",
                                  self->role_label(), self->client_ip_);
                    self->on_open();
                    self->do_read();
                }));
        return;
    }
    boost::asio::post(strand_, [self] {
        if (self->closed_.load()) return;
        self->on_open();
        self->do_read();
    });
}

void RelaySession::shutdown() {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self] { self->close("shutdown"); });
}

void RelaySession::close(const char* reason) {
    if (closed_.exchange(true)) return;
    spdlog::debug("[{} {}] disconnect: {}", role_label(), client_ip_, reason);
    writes_.stop();
    boost::system::error_code ec;
    socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket_.lowest_layer().close(ec);
    on_closed();
    if (rl_) rl_->on_disconnect(client_ip_);
    if (disconnect_handler_) disconnect_handler_(shared_from_this());
}

void RelaySession::send_line(std::string msg) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, m = std::move(msg)]() mutable {
        if (self->closed_.load()) return;
        self->writes_.push(std::move(m));
    });
}

void RelaySession::do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(
        boost::asio::buffer(read_buf_.data(), read_buf_.size()),
        boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code& ec, std::size_t n) {
                self->on_read(ec, n);
            }));
}

void RelaySession::on_read(const boost::system::error_code& ec, std::size_t n) {
    if (ec) { close(ec.message().c_str()); return; }
    buffer_.append(read_buf_.data(), n);
    // Cap the accumulation buffer to defeat slowloris-style memory attacks.
    if (buffer_.size() > (1u << 20)) { close("oversize buffer"); return; }
    while (true) {
        auto pos = buffer_.find('\n');
        if (pos == std::string::npos) break;
        std::string_view line(buffer_.data(), pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) on_line(line);
        buffer_.erase(0, pos + 1);
        if (closed_.load()) return;
    }
    if (!closed_.load()) do_read();
}

} // namespace mkpool
