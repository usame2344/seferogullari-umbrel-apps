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
// File:        cluster_protocol.hpp
// Description: mkpool-native cluster trunk protocol (passthrough/node <-> origin).
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

// The cluster trunk multiplexes many downstream Stratum V1 miners over a single
// TLS-capable connection between an edge (passthrough / node) and an origin pool.
// It is a NEW, versioned, mkpool-only protocol - deliberately not ckpool wire
// compatible - and is completely separate from the solo Stratum path.
//
// Framing: one compact JSON object per '\n'-terminated line, so it rides the exact
// same line reader the relay sessions already use. Message types:
//
//   edge -> origin
//     {"t":"hello","ver":1,"role":"passthrough"|"node","coin":"BTC","token":"..."}
//     {"t":"open","cid":N,"ip":"1.2.3.4"}        downstream miner connected
//     {"t":"data","cid":N,"l":"<stratum line>"}  one line from that miner
//     {"t":"close","cid":N}                      that miner disconnected
//     {"t":"pong"}
//
//   origin -> edge
//     {"t":"welcome","ver":1}                    hello accepted
//     {"t":"bye","msg":"..."}                    hello rejected (then close)
//     {"t":"data","cid":N,"l":"<stratum line>"}  one line for that miner
//     {"t":"close","cid":N}                      origin dropped that miner
//     {"t":"block","hex":"..","hash":"..","height":H}  node-only: submit locally
//     {"t":"ping"}
namespace mkpool::cluster {

inline constexpr int kProtocolVersion = 1;

using json = nlohmann::json;

// ---- builders (each returns a full frame line INCLUDING the trailing '\n') ----

inline std::string frame_hello(std::string_view role, std::string_view coin, std::string_view token) {
    json j{{"t", "hello"}, {"ver", kProtocolVersion}, {"role", role}, {"coin", coin}};
    if (!token.empty()) j["token"] = token;
    return j.dump() + "\n";
}
inline std::string frame_welcome() {
    return json{{"t", "welcome"}, {"ver", kProtocolVersion}}.dump() + "\n";
}
inline std::string frame_bye(std::string_view msg) {
    return json{{"t", "bye"}, {"msg", msg}}.dump() + "\n";
}
inline std::string frame_open(std::uint64_t cid, std::string_view ip) {
    return json{{"t", "open"}, {"cid", cid}, {"ip", ip}}.dump() + "\n";
}
inline std::string frame_data(std::uint64_t cid, std::string_view line) {
    return json{{"t", "data"}, {"cid", cid}, {"l", line}}.dump() + "\n";
}
inline std::string frame_close(std::uint64_t cid) {
    return json{{"t", "close"}, {"cid", cid}}.dump() + "\n";
}
inline std::string frame_block(std::string_view hex, std::string_view hash, std::int64_t height) {
    return json{{"t", "block"}, {"hex", hex}, {"hash", hash}, {"height", height}}.dump() + "\n";
}
inline std::string frame_ping() { return json{{"t", "ping"}}.dump() + "\n"; }
inline std::string frame_pong() { return json{{"t", "pong"}}.dump() + "\n"; }

// Parse one frame line. Returns false on malformed input or a missing "t".
inline bool parse(std::string_view line, json& out) {
    out = json::parse(line, nullptr, /*allow_exceptions=*/false);
    return out.is_object() && out.contains("t") && out["t"].is_string();
}

} // namespace mkpool::cluster
