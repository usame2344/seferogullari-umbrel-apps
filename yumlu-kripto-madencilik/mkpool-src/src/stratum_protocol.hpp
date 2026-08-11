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
// File:        stratum_protocol.hpp
// Description: Stratum V1 protocol parsing interface.
// Created:     2026-06-02
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace mkpool::stratum {

    inline constexpr const char* kMethodConfigure        = "mining.configure";
    inline constexpr const char* kMethodSubscribe        = "mining.subscribe";
    inline constexpr const char* kMethodAuthorize        = "mining.authorize";
    inline constexpr const char* kMethodSubmit           = "mining.submit";
    inline constexpr const char* kMethodExtranonceSub    = "mining.extranonce.subscribe";
    inline constexpr const char* kMethodSuggestDifficulty = "mining.suggest_difficulty";
    inline constexpr const char* kMethodSuggestTarget     = "mining.suggest_target";
    inline constexpr const char* kMethodMultiVersion      = "mining.multi_version";

    inline constexpr const char* kNotifyJob           = "mining.notify";
    inline constexpr const char* kNotifyDifficulty    = "mining.set_difficulty";
    inline constexpr const char* kNotifyVersionMask   = "mining.set_version_mask";
    inline constexpr const char* kNotifyExtranonce    = "mining.set_extranonce";

    // BIP310 negotiation result for a single session.
    struct ConfigureResult {
        std::uint32_t version_mask{0};         // 0 = version rolling disabled
        bool          version_rolling{false};           // granted (mask intersection != 0)
        bool          version_rolling_requested{false}; // miner asked for it
        bool          minimum_difficulty{false};        // miner asked for it
        double        suggested_min_difficulty{0.0};
        bool          subscribe_extranonce{false};      // miner asked for it
    };

    // Parse a mining.configure params object and produce the negotiated reply
    // params. `pool_version_mask` is the mask we support
    // (typically 0x1fffe000 per BIP320).
    [[nodiscard]] ConfigureResult
        negotiate_configure(const nlohmann::json& request_params,
                            std::uint32_t pool_version_mask) noexcept;

    // Validate a miner-submitted version against the negotiated mask.
    //
    // (submitted_version & ~mask) must equal (template_version & ~mask).
    [[nodiscard]] bool
        validate_version(std::uint32_t submitted_version,
                         std::uint32_t template_version,
                         std::uint32_t negotiated_mask) noexcept;

} // namespace mkpool::stratum
