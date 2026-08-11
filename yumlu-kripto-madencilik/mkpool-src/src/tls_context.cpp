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
// File:        tls_context.cpp
// Description: OpenSSL TLS context setup and certificate loading.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "tls_context.hpp"

#include <openssl/ssl.h>

namespace mkpool {

std::shared_ptr<boost::asio::ssl::context>
make_tls_context(const TlsConfig& cfg) {
    auto ctx = std::make_shared<boost::asio::ssl::context>(
        boost::asio::ssl::context::tlsv12);
    ctx->set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::no_sslv3 |
        boost::asio::ssl::context::no_tlsv1  |
        boost::asio::ssl::context::no_tlsv1_1 |
        boost::asio::ssl::context::single_dh_use);

    SSL_CTX* raw = ctx->native_handle();
    if (cfg.require_strict) {
        SSL_CTX_set_min_proto_version(raw, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(raw, TLS1_3_VERSION);
    }
    SSL_CTX_set_cipher_list(raw, cfg.ciphers.c_str());

    if (!cfg.cert_file.empty()) {
        ctx->use_certificate_chain_file(cfg.cert_file);
    }
    if (!cfg.key_file.empty()) {
        ctx->use_private_key_file(cfg.key_file, boost::asio::ssl::context::pem);
    }
    if (!cfg.dhparams_file.empty()) {
        ctx->use_tmp_dh_file(cfg.dhparams_file);
    }
    return ctx;
}

} // namespace mkpool
