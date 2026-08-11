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
// File:        any_stream.hpp
// Description: Type-erased Stratum transport over plain TCP, TLS and Stratum V2 Noise streams.
// Created:     2026-06-06
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <type_traits>
#include <utility>
#include <variant>

namespace mkpool {

class AnyStream {
public:
    using plain_t       = boost::asio::ip::tcp::socket;
    using tls_t         = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;
    using executor_type = boost::asio::any_io_executor;

    // Plain TCP.
    explicit AnyStream(boost::asio::io_context& io)
        : impl_(std::in_place_type<plain_t>, io) {}

    // TLS when `ctx` is non-null, otherwise plain TCP. The referenced context
    // must outlive this stream (the owning session keeps a shared_ptr to it).
    // Both alternatives are constructed in place (emplace), so this never
    // requires the variant - or ssl::stream - to be move-constructible.
    AnyStream(boost::asio::io_context& io, boost::asio::ssl::context* ctx)
        : impl_(std::in_place_type<plain_t>, io) {
        if (ctx) impl_.emplace<tls_t>(io, *ctx);
    }

    AnyStream(const AnyStream&)            = delete;
    AnyStream& operator=(const AnyStream&) = delete;

    [[nodiscard]] bool is_tls() const noexcept {
        return std::holds_alternative<tls_t>(impl_);
    }

    // The underlying TCP socket, for accept / endpoint / shutdown / close and
    // for the raw (unencrypted) Stratum V2 Noise path. For the TLS alternative
    // this is next_layer() (the tcp::socket), NOT lowest_layer() - the latter
    // returns the basic_socket base, which is not enough for async_accept.
    [[nodiscard]] plain_t& lowest_layer() noexcept {
        return std::visit([](auto& s) -> plain_t& {
            using S = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<S, plain_t>) return s;
            else                                      return s.next_layer();
        }, impl_);
    }

    [[nodiscard]] executor_type get_executor() noexcept {
        return lowest_layer().get_executor();
    }

    template <class MutableBufferSequence, class Token>
    auto async_read_some(const MutableBufferSequence& buffers, Token&& token) {
        return std::visit(
            [&](auto& s) {
                return s.async_read_some(buffers, std::forward<Token>(token));
            },
            impl_);
    }

    template <class ConstBufferSequence, class Token>
    auto async_write_some(const ConstBufferSequence& buffers, Token&& token) {
        return std::visit(
            [&](auto& s) {
                return s.async_write_some(buffers, std::forward<Token>(token));
            },
            impl_);
    }

    // Valid only on a TLS stream; callers must guard with is_tls().
    template <class Token>
    auto async_handshake(boost::asio::ssl::stream_base::handshake_type type,
                         Token&& token) {
        return std::get<tls_t>(impl_).async_handshake(type,
                                                      std::forward<Token>(token));
    }

private:
    using variant_t = std::variant<plain_t, tls_t>;
    variant_t impl_;
};

} // namespace mkpool
