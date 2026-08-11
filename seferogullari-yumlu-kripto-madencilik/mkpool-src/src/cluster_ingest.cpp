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
// File:        cluster_ingest.cpp
// Description: Origin-side cluster trunk acceptor: demux + loopback bridge.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "cluster_ingest.hpp"
#include "cluster_protocol.hpp"
#include "cluster_block_relay.hpp"

#include <spdlog/spdlog.h>

namespace mkpool {

ClusterIngestSession::ClusterIngestSession(boost::asio::io_context& io,
                                           std::shared_ptr<RateLimiter> rl,
                                           std::shared_ptr<boost::asio::ssl::context> tls_ctx,
                                           std::string forward_host,
                                           std::uint16_t forward_port,
                                           std::string coin,
                                           std::string token,
                                           std::shared_ptr<ClusterBlockRelay> block_relay)
    : RelaySession(io, std::move(rl), std::move(tls_ctx)),
      forward_host_(std::move(forward_host)),
      forward_port_(forward_port),
      coin_(std::move(coin)),
      token_(std::move(token)),
      block_relay_(std::move(block_relay)) {}

void ClusterIngestSession::deliver_block(const std::string& hex, const std::string& hash,
                                         std::int64_t height) {
    send_line(cluster::frame_block(hex, hash, height));
}

void ClusterIngestSession::on_line(std::string_view line) {
    json j;
    if (!cluster::parse(line, j)) { close("bad frame"); return; }
    const std::string t = j["t"].get<std::string>();

    if (!hello_ok_) {
        if (t != "hello") { send_line(cluster::frame_bye("expected hello")); close("no hello"); return; }
        handle_hello(j);
        return;
    }

    if (t == "open") {
        std::uint64_t cid = j.value("cid", std::uint64_t{0});
        if (cid) open_client(cid, j.value("ip", std::string{"?"}));
    } else if (t == "data") {
        std::uint64_t cid = j.value("cid", std::uint64_t{0});
        if (cid && j.contains("l") && j["l"].is_string())
            client_data(cid, j["l"].get<std::string>());
    } else if (t == "close") {
        std::uint64_t cid = j.value("cid", std::uint64_t{0});
        if (cid) close_client(cid, /*tell_edge=*/false);
    } else if (t == "ping") {
        send_line(cluster::frame_pong());
    }
    // "pong" and unknown types: ignore (keepalive / forward-compat).
}

void ClusterIngestSession::handle_hello(const json& j) {
    const std::string role  = j.value("role", std::string{});
    const std::string coin  = j.value("coin", std::string{});
    const std::string token = j.value("token", std::string{});

    if (!token_.empty() && token != token_) {
        spdlog::warn("[cluster-ingest {}] rejected trunk: bad token", client_ip());
        send_line(cluster::frame_bye("auth failed")); close("bad token"); return;
    }
    if (!coin_.empty() && !coin.empty() && coin != coin_) {
        send_line(cluster::frame_bye("coin mismatch")); close("coin mismatch"); return;
    }
    if (role != "passthrough" && role != "node") {
        send_line(cluster::frame_bye("bad role")); close("bad role"); return;
    }

    hello_ok_ = true;
    is_node_  = (role == "node");
    send_line(cluster::frame_welcome());
    spdlog::info("[cluster-ingest {}] trunk up role={} coin={} -> {}:{}",
                 client_ip(), role, coin.empty() ? "*" : coin, forward_host_, forward_port_);

    // Node trunks additionally receive origin-found blocks for local submission.
    if (is_node_ && block_relay_) {
        block_relay_->register_node(
            std::static_pointer_cast<ClusterIngestSession>(shared_from_this()));
        spdlog::info("[cluster-ingest {}] registered node trunk for block relay", client_ip());
    }
}

void ClusterIngestSession::open_client(std::uint64_t cid, std::string ip) {
    if (clients_.count(cid)) return;  // duplicate open; ignore

    auto self = std::static_pointer_cast<ClusterIngestSession>(shared_from_this());
    std::weak_ptr<ClusterIngestSession> weak = self;

    auto conn = std::make_shared<UpstreamConn>(
        io_, forward_host_, std::to_string(forward_port_),
        /*tls=*/false, /*tls_ctx=*/nullptr,
        // origin -> miner: wrap each stratum line in a data frame and send it up.
        [weak, cid](std::string_view l) {
            if (auto s = weak.lock()) s->send_line(cluster::frame_data(cid, l));
        },
        // loopback connected: flush anything buffered before connect.
        [weak, cid]() {
            if (auto s = weak.lock())
                boost::asio::post(s->strand(), [s, cid] { s->loopback_connected(cid); });
        },
        // loopback closed: tell the edge and drop the bridge.
        [weak, cid](const std::string& /*reason*/) {
            if (auto s = weak.lock())
                boost::asio::post(s->strand(), [s, cid] { s->close_client(cid, /*tell_edge=*/true); });
        });

    Client c;
    c.conn = conn;
    clients_.emplace(cid, std::move(c));
    ++clients_seen_;
    spdlog::debug("[cluster-ingest {}] open cid={} ip={} (bridging to {}:{})",
                  client_ip(), cid, ip, forward_host_, forward_port_);
    conn->start();
}

void ClusterIngestSession::loopback_connected(std::uint64_t cid) {
    auto it = clients_.find(cid);
    if (it == clients_.end()) return;
    it->second.connected = true;
    for (auto& l : it->second.pending) it->second.conn->send(std::move(l));
    it->second.pending.clear();
}

void ClusterIngestSession::client_data(std::uint64_t cid, std::string line) {
    auto it = clients_.find(cid);
    if (it == clients_.end()) return;  // unknown / already closed
    line.push_back('\n');
    if (it->second.connected) {
        it->second.conn->send(std::move(line));
    } else if (it->second.pending.size() < 64) {
        // Bound the pre-connect buffer; a healthy loopback drains it in a few ms.
        it->second.pending.push_back(std::move(line));
    }
}

void ClusterIngestSession::close_client(std::uint64_t cid, bool tell_edge) {
    auto it = clients_.find(cid);
    if (it == clients_.end()) return;
    if (it->second.conn) it->second.conn->close("cluster client closed");
    clients_.erase(it);
    if (tell_edge && !closed_.load()) send_line(cluster::frame_close(cid));
}

void ClusterIngestSession::on_closed() {
    // The trunk is gone: drop every bridged loopback connection.
    for (auto& [cid, c] : clients_)
        if (c.conn) c.conn->close("trunk closed");
    clients_.clear();
}

} // namespace mkpool
