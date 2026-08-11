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
// File:        test_stratum_configure.cpp
// Description: Unit tests for Stratum V1 mining.configure (BIP310).
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include <catch2/catch_test_macros.hpp>
#include "stratum_protocol.hpp"

#include <nlohmann/json.hpp>

using mkpool::stratum::negotiate_configure;
using mkpool::stratum::validate_version;

TEST_CASE("version-rolling negotiated to intersection of masks", "[stratum][bip310]") {
    nlohmann::json params = {{"version-rolling", "minimum-difficulty"},
                              {{"version-rolling.mask", "1fffe000"},
                               {"minimum-difficulty.value", 1}}};
    // Note: nlohmann inits this as object due to mixed entries; if not,
    // fall back to a hand-built params array.
    nlohmann::json fixed_params = nlohmann::json::array({
        nlohmann::json::array({"version-rolling", "minimum-difficulty"}),
        nlohmann::json::object({
            {"version-rolling.mask", "1fffe000"},
            {"minimum-difficulty.value", 1}
        })
    });
    auto r = negotiate_configure(fixed_params, 0x1fffe000u);
    CHECK(r.version_rolling);
    CHECK(r.version_mask == 0x1fffe000u);
}

TEST_CASE("version bits outside mask rejected", "[stratum][version]") {
    // template version: 0x20000000, mask: 0x1fffe000 (covers bits 13-28).
    // Valid: submitted may only flip bits inside the mask; out-of-mask bits
    // must equal those in the template.
    CHECK(validate_version(0x20002000u, 0x20000000u, 0x1fffe000u));
    // 0x40000000 sets bit 30 which is outside the mask -> must be rejected.
    CHECK_FALSE(validate_version(0x40000000u, 0x20000000u, 0x1fffe000u));
}

TEST_CASE("no version-rolling negotiated when pool mask is zero", "[stratum][bip310]") {
    nlohmann::json params = nlohmann::json::array({
        nlohmann::json::array({"version-rolling"}),
        nlohmann::json::object({{"version-rolling.mask", "1fffe000"}})
    });
    auto r = negotiate_configure(params, 0u);
    CHECK_FALSE(r.version_rolling);
    CHECK(r.version_mask == 0u);
}
