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
// File:        tls_context.hpp
// Description: OpenSSL TLS context interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <boost/asio/ssl.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mkpool {

    struct TlsConfig {
        std::string cert_file;       // PEM
        std::string key_file;        // PEM
        std::string dhparams_file;   // optional
        std::string ciphers{
            "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:"
            "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256"};
        bool require_strict{true};
    };

    // Returns an asio ssl::context configured for modern TLS 1.2/1.3 only.
    [[nodiscard]] std::shared_ptr<boost::asio::ssl::context>
        make_tls_context(const TlsConfig& cfg);

    // A swappable holder for a TLS context, so a SIGHUP-triggered certificate
    // reload can install a freshly-loaded context for *new* handshakes without
    // disturbing in-flight sessions (each session keeps a shared_ptr to the
    // context it handshook with, so the old one stays alive until they close).
    class TlsReloadable {
    public:
        void set(std::shared_ptr<boost::asio::ssl::context> ctx) {
            std::lock_guard<std::mutex> lk(mu_);
            ctx_ = std::move(ctx);
        }
        [[nodiscard]] std::shared_ptr<boost::asio::ssl::context> get() const {
            std::lock_guard<std::mutex> lk(mu_);
            return ctx_;
        }

    private:
        mutable std::mutex mu_;
        std::shared_ptr<boost::asio::ssl::context> ctx_;
    };

} // namespace mkpool
