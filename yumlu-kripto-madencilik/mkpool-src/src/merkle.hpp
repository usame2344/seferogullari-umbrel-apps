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
// File:        merkle.hpp
// Description: Merkle branch construction interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mkpool::merkle {

    using Hash = std::array<std::uint8_t, 32>;

    // Compute SHA256d (double SHA-256).
    [[nodiscard]] Hash sha256d(std::span<const std::uint8_t> data);

    // Build the merkle branch path for the coinbase transaction.
    //
    // Bitcoin Stratum convention: branch elements are LITTLE-ENDIAN txids
    // (i.e. the natural display reversal of internal hash bytes).
    //
    // `txids_internal` must contain non-coinbase transaction ids in *internal*
    // byte order (little-endian display reversed -> internal). The coinbase txid
    // is the first leaf and is intentionally omitted because the miner / pool
    // recomputes it after assembling extranonce2.
    //
    // Returns a list of 32-byte siblings as little-endian (stratum) hex strings.
    [[nodiscard]] std::vector<std::string>
        build_branch_le_hex(std::span<const std::string> txids_le_hex);

    // Recompute merkle root from a coinbase txid (LE hex) and a precomputed branch
    // (LE hex). Returns LE hex (32 bytes).
    [[nodiscard]] std::string
        compute_root_le_hex(std::string_view coinbase_txid_le_hex,
                            std::span<const std::string> branch_le_hex);

} // namespace mkpool::merkle
