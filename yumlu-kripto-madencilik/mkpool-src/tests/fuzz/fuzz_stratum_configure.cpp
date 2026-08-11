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
// File:        fuzz_stratum_configure.cpp
// Description: libFuzzer target for mining.configure negotiation (Stratum V1).
// Created:     2026-07-15
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool
//
// mining.configure precedes authorize, so params reach negotiate_configure()
// from an unauthenticated peer. process_line() guarantees only that the frame
// parsed as JSON and has a string "method"; params can be any JSON value.
//
// Contracts under test:
//  1. negotiate_configure() is noexcept -- nlohmann throws type_error on a bad
//     .get<T>(), and escaping that is std::terminate, not an error return.
//  2. The negotiated mask never contains a bit outside the pool's own mask,
//     whatever the miner asks for.

#include "stratum_protocol.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

namespace {

// BIP320 mask the pool advertises in production.
constexpr std::uint32_t kPoolMask = 0x1fffe000u;

void check(const nlohmann::json& params, std::uint32_t pool_mask) {
    // Contract 1: noexcept. If this throws, the process is already gone.
    const auto r = mkpool::stratum::negotiate_configure(params, pool_mask);

    // Contract 2: never grant a bit outside the pool mask.
    if ((r.version_mask & ~pool_mask) != 0) __builtin_trap();

    // A mask can only be non-zero if rolling was actually granted, and rolling
    // can only be granted if the miner asked: no silent self-enabling.
    if (r.version_mask != 0 && !r.version_rolling) __builtin_trap();
    if (r.version_rolling && !r.version_rolling_requested) __builtin_trap();

    // Any version we would accept must survive our own validator, or the two
    // halves of BIP310 disagree and honest miners get their shares rejected.
    if (r.version_rolling) {
        constexpr std::uint32_t tmpl = 0x20000000u;
        if (!mkpool::stratum::validate_version(tmpl, tmpl, r.version_mask)) __builtin_trap();
        if (!mkpool::stratum::validate_version(tmpl ^ r.version_mask, tmpl, r.version_mask)) __builtin_trap();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1 || size > 65536) return 0;

    // Byte 0 varies the pool mask so the intersection logic is exercised against
    // more than one configuration; the rest is the JSON frame.
    const std::uint32_t pool_mask = (data[0] & 1) ? kPoolMask
                                 : (data[0] & 2) ? 0u
                                 : (static_cast<std::uint32_t>(data[0]) << 13);

    const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);

    nlohmann::json params;
    try {
        // Mirror process_line(): parse first, and only structurally valid JSON
        // ever reaches the handler.
        params = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        return 0; // a parse failure is handled upstream, not our surface
    }

    check(params, pool_mask);
    return 0;
}
