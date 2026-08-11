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
// File:        cluster_trunk.cpp
// Description: Edge-side (passthrough/node) cluster trunk + downstream session.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "cluster_trunk.hpp"
#include "cluster_protocol.hpp"
#include "upstream_client.hpp"
#include "bitcoin_client.hpp"

#include <algorithm>
#include <vector>
#include <spdlog/spdlog.h>

namespace mkpool {

// =========================================================================
// ClusterEdgeSession - one downstream miner at the edge
// =========================================================================

ClusterEdgeSession::ClusterEdgeSession(boost::asio::io_context& io,
                                       std::shared_ptr<RateLimiter> rl,
                                       std::shared_ptr<ClusterTrunk> trunk,
                                       std::shared_ptr<boost::asio::ssl::context> tls_ctx)
    : RelaySession(io, std::move(rl), std::move(tls_ctx)),
      trunk_(std::move(trunk)) {}

void ClusterEdgeSession::on_open() {
    auto self = std::static_pointer_cast<ClusterEdgeSession>(shared_from_this());
    cid_ = trunk_->attach(self, client_ip());
}

void ClusterEdgeSession::on_line(std::string_view line) {
    if (cid_) trunk_->client_line(cid_, std::string(line));  // miner -> origin
}

void ClusterEdgeSession::deliver(std::string_view line) {   // origin -> miner
    std::string m;
    m.reserve(line.size() + 1);
    m.assign(line.data(), line.size());
    m.push_back('\n');
    send_line(std::move(m));
}

void ClusterEdgeSession::on_closed() {
    if (cid_ && trunk_) trunk_->detach(cid_);
}

// =========================================================================
// ClusterTrunk - outbound multiplexed uplink to the origin
// =========================================================================

ClusterTrunk::ClusterTrunk(boost::asio::io_context& io, CoinConfig coin, bool node_role)
    : io_(io),
      strand_(boost::asio::make_strand(io.get_executor())),
      coin_(std::move(coin)),
      node_role_(node_role),
      reconnect_timer_(io) {
    if (node_role_) {
        // Node role: a client to the LOCAL bitcoind for redundant/fast block
        // submission of blocks the origin streams down the trunk.
        local_node_ = std::make_shared<bitcoin::BitcoinClient>(
            io_, coin_.rpcHost, coin_.rpcPort, coin_.rpcUser, coin_.rpcPassword,
            coin_.chain == ChainKind::BitcoinCash, coin_.chain);
    }
}

void ClusterTrunk::start() {
    running_.store(true);
    boost::asio::post(strand_, [self = shared_from_this()] { self->connect(); });
}

void ClusterTrunk::stop() {
    running_.store(false);
    boost::asio::post(strand_, [self = shared_from_this()] {
        self->reconnect_timer_.cancel();
        if (self->conn_) self->conn_->close("stop");
        self->conn_.reset();
        self->pending_out_.clear();
    });
    drop_all_sessions("trunk stopped");
}

void ClusterTrunk::connect() {
    if (!running_.load()) return;
    welcomed_.store(false);
    const auto& cl = coin_.cluster;

    std::shared_ptr<boost::asio::ssl::context> ctx;
    if (cl.originTls) {
        ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
        ctx->set_options(boost::asio::ssl::context::default_workarounds |
                         boost::asio::ssl::context::no_sslv2 |
                         boost::asio::ssl::context::no_sslv3);
        if (cl.originVerifyPeer) { ctx->set_default_verify_paths(); ctx->set_verify_mode(boost::asio::ssl::verify_peer); }
        else                     { ctx->set_verify_mode(boost::asio::ssl::verify_none); }
    }

    std::weak_ptr<ClusterTrunk> weak = shared_from_this();
    conn_ = std::make_shared<UpstreamConn>(
        io_, cl.originHost, std::to_string(cl.originPort), cl.originTls, ctx,
        [weak](std::string_view l) {
            if (auto s = weak.lock())
                boost::asio::post(s->strand_, [s, line = std::string(l)] { s->on_frame(line); });
        },
        [weak]() {
            if (auto s = weak.lock())
                boost::asio::post(s->strand_, [s] { s->on_open(); });
        },
        [weak](const std::string& r) {
            if (auto s = weak.lock())
                boost::asio::post(s->strand_, [s, r] { s->on_close(r); });
        });
    conn_->start();
}

void ClusterTrunk::on_open() {
    reconnect_backoff_ = 0;
    const auto& cl = coin_.cluster;
    const char* role = node_role_ ? "node" : "passthrough";
    if (conn_) conn_->send(cluster::frame_hello(role, coin_.name, cl.token));
    spdlog::info("[cluster-trunk {}] connected to {}:{}; hello role={}",
                 coin_.name, cl.originHost, cl.originPort, role);
}

void ClusterTrunk::on_frame(std::string_view line) {
    nlohmann::json j;
    if (!cluster::parse(line, j)) return;
    const std::string t = j["t"].get<std::string>();

    if (t == "welcome") {
        welcomed_.store(true);
        for (auto& f : pending_out_) if (conn_) conn_->send(std::move(f));
        pending_out_.clear();
        spdlog::info("[cluster-trunk {}] welcomed by origin (flushed {} buffered frames)",
                     coin_.name, 0);
    } else if (t == "bye") {
        spdlog::error("[cluster-trunk {}] origin rejected trunk: {}",
                      coin_.name, j.value("msg", std::string{"?"}));
        if (conn_) conn_->close("bye");
    } else if (t == "data") {
        std::uint64_t cid = j.value("cid", std::uint64_t{0});
        if (cid && j.contains("l") && j["l"].is_string()) {
            std::shared_ptr<ClusterEdgeSession> s;
            {
                std::lock_guard lk(mu_);
                auto it = sessions_.find(cid);
                if (it != sessions_.end()) s = it->second.lock();
            }
            if (s) s->deliver(j["l"].get<std::string>());
        }
    } else if (t == "close") {
        std::uint64_t cid = j.value("cid", std::uint64_t{0});
        std::shared_ptr<ClusterEdgeSession> s;
        {
            std::lock_guard lk(mu_);
            auto it = sessions_.find(cid);
            if (it != sessions_.end()) { s = it->second.lock(); sessions_.erase(it); }
        }
        if (s) s->shutdown();
    } else if (t == "block") {
        submit_block(j.value("hex", std::string{}), j.value("hash", std::string{}),
                     j.value("height", std::int64_t{0}));
    } else if (t == "ping") {
        if (conn_) conn_->send(cluster::frame_pong());
    }
    // "pong" / unknown: ignore.
}

void ClusterTrunk::on_close(const std::string& reason) {
    welcomed_.store(false);
    conn_.reset();
    pending_out_.clear();
    drop_all_sessions("trunk lost");
    if (running_.load()) {
        stat_reconnects_.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("[cluster-trunk {}] trunk down ({}); reconnecting", coin_.name, reason);
        schedule_reconnect();
    }
}

void ClusterTrunk::schedule_reconnect() {
    const int backoff = std::min(30, 2 + reconnect_backoff_ * 2);
    reconnect_backoff_ = std::min(reconnect_backoff_ + 1, 15);
    reconnect_timer_.expires_after(std::chrono::seconds(backoff));
    reconnect_timer_.async_wait(boost::asio::bind_executor(strand_,
        [self = shared_from_this()](const boost::system::error_code& ec) {
            if (!ec && self->running_.load()) self->connect();
        }));
}

void ClusterTrunk::send_frame(std::string frame) {
    boost::asio::post(strand_, [self = shared_from_this(), f = std::move(frame)]() mutable {
        if (!self->running_.load()) return;
        if (self->welcomed_.load() && self->conn_) {
            self->conn_->send(std::move(f));
        } else if (self->pending_out_.size() < 100000) {
            self->pending_out_.push_back(std::move(f));
        }
        // else: trunk down with a huge backlog; the session is dropped on
        // reconnect anyway, so discarding is correct (never blocks a miner).
    });
}

std::uint64_t ClusterTrunk::attach(const std::weak_ptr<ClusterEdgeSession>& s,
                                   const std::string& ip) {
    const std::uint64_t cid = next_cid_.fetch_add(1, std::memory_order_relaxed);
    { std::lock_guard lk(mu_); sessions_[cid] = s; }
    stat_opened_.fetch_add(1, std::memory_order_relaxed);
    send_frame(cluster::frame_open(cid, ip));
    return cid;
}

void ClusterTrunk::client_line(std::uint64_t cid, std::string line) {
    send_frame(cluster::frame_data(cid, line));
}

void ClusterTrunk::detach(std::uint64_t cid) {
    bool had = false;
    { std::lock_guard lk(mu_); had = sessions_.erase(cid) > 0; }
    if (had) {
        stat_closed_.fetch_add(1, std::memory_order_relaxed);
        send_frame(cluster::frame_close(cid));
    }
}

void ClusterTrunk::drop_all_sessions(const char* /*reason*/) {
    std::vector<std::shared_ptr<ClusterEdgeSession>> snap;
    {
        std::lock_guard lk(mu_);
        snap.reserve(sessions_.size());
        for (auto& [cid, w] : sessions_) if (auto s = w.lock()) snap.push_back(std::move(s));
        sessions_.clear();
    }
    for (auto& s : snap) s->shutdown();
}

void ClusterTrunk::submit_block(const std::string& hex, const std::string& hash,
                                std::int64_t height) {
    stat_blocks_.fetch_add(1, std::memory_order_relaxed);
    if (!node_role_ || !local_node_ || hex.empty()) return;
    spdlog::info("[cluster-node {}] submitting origin block height={} hash={} to local node",
                 coin_.name, height, hash);
    local_node_->asyncSubmitBlock(hex,
        [coin = coin_.name, hash, height](const boost::system::error_code& ec,
                                          const nlohmann::json& res) {
            if (ec) {
                spdlog::warn("[cluster-node {}] local submitblock transport error height={}: {}",
                             coin, height, ec.message());
                return;
            }
            // bitcoind: null result == accepted; a string == reason
            // ("duplicate"/"inconclusive" is normal - p2p may have delivered it first).
            if (res.is_null())
                spdlog::info("[cluster-node {}] local node ACCEPTED block height={} hash={}",
                             coin, height, hash);
            else
                spdlog::info("[cluster-node {}] local node returned '{}' for block height={} "
                             "(already-known is expected)", coin, res.dump(), height);
        });
}

} // namespace mkpool
