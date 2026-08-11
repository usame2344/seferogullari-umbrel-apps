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
// File:        cluster_block_relay.cpp
// Description: Origin-side fan-out of found blocks to connected node trunks.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "cluster_block_relay.hpp"
#include "cluster_ingest.hpp"
#include "zmq_client.hpp"
#include "utils.hpp"

#include <span>
#include <spdlog/spdlog.h>

namespace mkpool {

namespace {
// Read a Bitcoin varint at block[p], advancing p. Bounds-checked: returns false
// (leaving p unchanged) if the buffer is too short.
bool read_varint(const std::string& b, std::size_t& p, std::uint64_t& out) {
    if (p >= b.size()) return false;
    const std::uint8_t c = static_cast<std::uint8_t>(b[p]);
    auto need = [&](std::size_t n) { return p + n <= b.size(); };
    if (c < 0xfd)      { out = c; p += 1; return true; }
    if (c == 0xfd)     { if (!need(3)) return false; out = (std::uint8_t)b[p+1] | ((std::uint64_t)(std::uint8_t)b[p+2] << 8); p += 3; return true; }
    if (c == 0xfe)     { if (!need(5)) return false; out = 0; for (int i = 0; i < 4; ++i) out |= (std::uint64_t)(std::uint8_t)b[p+1+i] << (8*i); p += 5; return true; }
    if (!need(9)) return false;
    out = 0; for (int i = 0; i < 8; ++i) out |= (std::uint64_t)(std::uint8_t)b[p+1+i] << (8*i); p += 9; return true;
}

// Best-effort BIP34 coinbase height, for logging only. 0 if it cannot be parsed.
std::int64_t bip34_height(const std::string& block) {
    std::size_t p = 80;                 // skip the 80-byte header
    std::uint64_t txcount = 0, vin = 0, scriptlen = 0;
    if (!read_varint(block, p, txcount)) return 0;
    p += 4;                              // coinbase tx version
    // In a segwit block the coinbase tx is serialized with a witness, so a
    // marker(0x00)+flag(0x01) sits between version and the input count. A real
    // input count is never 0, so a 0x00 here is unambiguously the marker.
    if (p + 1 < block.size() &&
        static_cast<std::uint8_t>(block[p]) == 0x00 &&
        static_cast<std::uint8_t>(block[p+1]) == 0x01) {
        p += 2;
    }
    if (!read_varint(block, p, vin)) return 0;
    p += 36;                             // prevout hash(32) + index(4)
    if (!read_varint(block, p, scriptlen)) return 0;
    if (p >= block.size()) return 0;
    const std::uint8_t hlen = static_cast<std::uint8_t>(block[p++]);
    if (hlen == 0 || hlen > 7 || p + hlen > block.size()) return 0;
    std::int64_t h = 0;
    for (int i = 0; i < hlen; ++i) h |= (std::int64_t)(std::uint8_t)block[p+i] << (8*i);
    return h;
}
} // namespace

ClusterBlockRelay::ClusterBlockRelay(boost::asio::io_context& io,
                                     std::string zmq_endpoint, std::string coin)
    : io_(io), zmq_endpoint_(std::move(zmq_endpoint)), coin_(std::move(coin)) {}

void ClusterBlockRelay::start() {
    zmq_ = std::make_shared<ZMQClient>(io_, zmq_endpoint_);
    zmq_->subscribe("rawblock");
    auto self = shared_from_this();
    zmq_->start([self](const std::string& topic, const std::string& data) {
        if (topic == "rawblock") self->on_rawblock(data);
    });
    spdlog::info("[cluster-relay {}] rawblock feed at {} -> node trunks", coin_, zmq_endpoint_);
}

void ClusterBlockRelay::stop() {
    if (zmq_) zmq_->stop();
}

void ClusterBlockRelay::register_node(const std::weak_ptr<ClusterIngestSession>& s) {
    std::lock_guard lk(mu_);
    nodes_.push_back(s);
}

void ClusterBlockRelay::on_rawblock(const std::string& raw) {
    if (raw.size() < 80) return;
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(raw.data());

    auto hh = utils::sha256d_arr(std::span<const std::uint8_t>{p, 80});
    const std::string hash   = utils::byte_reverse_hex(utils::bytes_to_hex(hh));
    const std::int64_t height = bip34_height(raw);
    const std::string hex    = utils::bytes_to_hex(std::span<const std::uint8_t>{p, raw.size()});

    // Snapshot the live node trunks, compacting expired weak_ptrs as we go.
    std::vector<std::shared_ptr<ClusterIngestSession>> live;
    {
        std::lock_guard lk(mu_);
        std::vector<std::weak_ptr<ClusterIngestSession>> keep;
        keep.reserve(nodes_.size());
        for (auto& w : nodes_) if (auto s = w.lock()) { live.push_back(s); keep.push_back(w); }
        nodes_.swap(keep);
        ++blocks_relayed_;
    }
    if (live.empty()) return;

    spdlog::info("[cluster-relay {}] streaming block height={} hash={} to {} node trunk(s)",
                 coin_, height, hash, live.size());
    for (auto& s : live) s->deliver_block(hex, hash, height);
}

} // namespace mkpool
