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
// File:        relay_session.hpp
// Description: Shared downstream transport base for the scale-out relay roles.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "any_stream.hpp"
#include "rate_limiter.hpp"
#include "write_queue.hpp"

namespace mkpool {

// Transport base for a downstream miner connection in a relay role (proxy /
// redirector). It owns the plain-or-TLS socket, the strand-serialised write
// queue and the Stratum V1 line reader, and provides the same descriptor-safety
// discipline as the solo ClientSession (write chain bound to the session strand,
// self-keeper weak_ptr, watermark drop) without touching any solo code.
//
// Subclasses implement on_open() (post-handshake hook) and on_line() (one
// Stratum V1 request line). This base deliberately does NOT speak Stratum V2:
// the stage-1 relay roles serve V1 (+ optional TLS) miners, which covers every
// ASIC and aggregator in the field; SV2-downstream is a later addition.
class RelaySession : public std::enable_shared_from_this<RelaySession> {
public:
    using SocketT           = AnyStream;
    using Strand            = boost::asio::strand<boost::asio::any_io_executor>;
    using DisconnectHandler = std::function<void(const std::shared_ptr<RelaySession>&)>;

    // `tls_ctx` non-null makes this a TLS-terminating session. The shared_ptr is
    // retained for the session's lifetime so the OpenSSL context outlives the
    // stream, exactly as the solo path does.
    RelaySession(boost::asio::io_context& io,
                 std::shared_ptr<RateLimiter> rl,
                 std::shared_ptr<boost::asio::ssl::context> tls_ctx);

    virtual ~RelaySession() = default;

    // The raw TCP socket, used by the listener for async_accept / endpoint / close.
    boost::asio::ip::tcp::socket& socket() { return socket_.lowest_layer(); }
    [[nodiscard]] const std::string& client_ip() const { return client_ip_; }

    // Begin serving. Called once after the socket is accepted.
    void start();

    // Safe to call from any thread; posts teardown to the strand.
    void shutdown();

    void setDisconnectHandler(DisconnectHandler h) { disconnect_handler_ = std::move(h); }
    Strand& strand() { return strand_; }

    [[nodiscard]] bool is_tls() const noexcept { return is_tls_; }

protected:
    // Hook invoked on the strand once the transport (incl. TLS) is ready and the
    // read loop has been armed. Default: nothing.
    virtual void on_open() {}

    // Handle one Stratum V1 request line (no trailing newline). Runs on the strand.
    virtual void on_line(std::string_view line) = 0;

    // A short label for logs ("proxy", "redirector", ...). Default "relay".
    [[nodiscard]] virtual const char* role_label() const noexcept { return "relay"; }

    // Invoked once from close(), on the strand, after the socket is torn down.
    // Subclasses release external registrations here (e.g. an extranonce slot).
    virtual void on_closed() {}

    // Enqueue an outbound line (strand-posted). Appends nothing: callers include
    // any trailing newline they need.
    void send_line(std::string msg);

    // Tear the session down (idempotent). Runs on / posts to the strand.
    void close(const char* reason);

    boost::asio::io_context& io_;
    Strand    strand_;
    // Declared before socket_ so the OpenSSL context (referenced by the TLS
    // stream inside socket_) is destroyed after the stream.
    std::shared_ptr<boost::asio::ssl::context> tls_ctx_;
    bool      is_tls_{false};
    SocketT   socket_;
    WriteQueue<SocketT> writes_;
    std::shared_ptr<RateLimiter> rl_;

    std::string client_ip_;
    std::atomic<bool> closed_{false};

private:
    void do_read();
    void on_read(const boost::system::error_code& ec, std::size_t bytes);

    std::array<char, 4096> read_buf_{};
    std::string buffer_;
    DisconnectHandler disconnect_handler_;
};

} // namespace mkpool
