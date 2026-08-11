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
// File:        stratum_protocol.cpp
// Description: Stratum V1 message parsing and BIP310 mining.configure handling.
// Created:     2026-06-02
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "stratum_protocol.hpp"

#include <algorithm>

namespace mkpool::stratum {

// Internal negotiator. MAY THROW -- see the negotiate_configure() wrapper below,
// which is the noexcept boundary. Keep the json poking in here, not there.
static ConfigureResult negotiate_configure_impl(const nlohmann::json& params,
                                                std::uint32_t pool_version_mask) {
    ConfigureResult r;
    // params shape:
    // [
    //   [ "version-rolling", "minimum-difficulty", "subscribe-extranonce" ],
    //   {
    //     "version-rolling.mask": "1fffe000",
    //     "version-rolling.min-bit-count": 16,
    //     "minimum-difficulty.value": 1
    //   }
    // ]
    if (!params.is_array() || params.size() < 2) return r;
    const auto& features = params[0];
    const auto& extra    = params[1];
    if (!features.is_array() || !extra.is_object()) return r;

    auto has_feature = [&](std::string_view name) {
        for (const auto& f : features) {
            if (f.is_string() && f.get_ref<const std::string&>() == name) return true;
        }
        return false;
    };

    if (has_feature("version-rolling")) {
        r.version_rolling_requested = true;
        std::uint32_t miner_mask = 0xffffffffu;
        auto it = extra.find("version-rolling.mask");
        if (it != extra.end() && it->is_string()) {
            try {
                miner_mask = std::stoul(it->get<std::string>(), nullptr, 16);
            } catch (...) { miner_mask = 0; }
        }
        const std::uint32_t intersection = miner_mask & pool_version_mask;
        if (intersection != 0) {
            r.version_rolling = true;
            r.version_mask    = intersection;
        }
    }
    if (has_feature("minimum-difficulty")) {
        r.minimum_difficulty = true;
        auto it = extra.find("minimum-difficulty.value");
        if (it != extra.end() && it->is_number()) {
            r.suggested_min_difficulty = it->get<double>();
        }
    }
    if (has_feature("subscribe-extranonce")) {
        r.subscribe_extranonce = true;
    }
    return r;
}

// noexcept boundary. `params` is arbitrary JSON from an unauthenticated peer
// (mining.configure precedes authorize), and nlohmann throws type_error on a
// mistyped .get<T>(); escaping that here is std::terminate, not an error return.
// A default-constructed result grants nothing, which is the safe outcome.
ConfigureResult negotiate_configure(const nlohmann::json& params,
                                    std::uint32_t pool_version_mask) noexcept {
    try {
        return negotiate_configure_impl(params, pool_version_mask);
    } catch (...) {
        return ConfigureResult{};
    }
}

// build_configure_reply removed: client_session.cpp::handle_configure now
// builds the BIP310 reply directly via fmt::format (no nlohmann allocs).



bool validate_version(std::uint32_t submitted, std::uint32_t templ,
                      std::uint32_t mask) noexcept {
    return (submitted & ~mask) == (templ & ~mask);
}

} // namespace mkpool::stratum
