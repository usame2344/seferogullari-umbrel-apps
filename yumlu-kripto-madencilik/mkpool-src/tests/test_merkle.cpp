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
// File:        test_merkle.cpp
// Description: Unit tests for merkle branch construction.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include <catch2/catch_test_macros.hpp>
#include "merkle.hpp"
#include "utils.hpp"

#include <array>
#include <string>
#include <vector>

using mkpool::merkle::build_branch_le_hex;
using mkpool::merkle::compute_root_le_hex;
using mkpool::utils::hex_to_bytes;
using mkpool::utils::sha256d_arr;
using mkpool::utils::bytes_to_hex;

// helper: compute LE-hex merkle root of a list of LE-hex txids (slot 0 = coinbase).
// Bitcoin merkle internals concatenate the raw bytes (== "LE bytes" in this
// codebase's convention) directly; reversal is purely a display concern.
static std::string brute_root(const std::vector<std::string>& txids_le) {
    std::vector<std::array<std::uint8_t,32>> lvl;
    lvl.reserve(txids_le.size());
    for (auto& h : txids_le) {
        auto v = hex_to_bytes(h);
        std::array<std::uint8_t,32> a{};
        std::memcpy(a.data(), v.data(), 32);
        lvl.push_back(a);
    }
    while (lvl.size() > 1) {
        if (lvl.size() % 2) lvl.push_back(lvl.back());
        std::vector<std::array<std::uint8_t,32>> next;
        next.reserve(lvl.size() / 2);
        for (std::size_t i = 0; i < lvl.size(); i += 2) {
            std::array<std::uint8_t, 64> cat{};
            std::memcpy(cat.data(),      lvl[i].data(),     32);
            std::memcpy(cat.data() + 32, lvl[i + 1].data(), 32);
            next.push_back(sha256d_arr({cat.data(), cat.size()}));
        }
        lvl = std::move(next);
    }
    return bytes_to_hex({lvl[0].data(), lvl[0].size()});
}

TEST_CASE("single coinbase tx -> empty branch, root = coinbase txid", "[merkle]") {
    // Production API: build_branch_le_hex receives the non-coinbase tx list.
    // For a single-coinbase block that list is empty.
    const std::string coinbase_txid = "aa" + std::string(62, '0');
    std::vector<std::string> other_txids;
    auto branch = build_branch_le_hex(other_txids);
    CHECK(branch.empty());
    CHECK(compute_root_le_hex(coinbase_txid, branch) == coinbase_txid);
}

TEST_CASE("branch reconstruction yields canonical root", "[merkle]") {
    // coinbase + 4 fake txids (txids[0] is the coinbase placeholder for brute_root)
    std::vector<std::string> txids = {
        "1100000000000000000000000000000000000000000000000000000000000000",
        "2200000000000000000000000000000000000000000000000000000000000000",
        "3300000000000000000000000000000000000000000000000000000000000000",
        "4400000000000000000000000000000000000000000000000000000000000000",
        "5500000000000000000000000000000000000000000000000000000000000000",
    };
    std::vector<std::string> other_txids(txids.begin() + 1, txids.end());
    auto branch = build_branch_le_hex(other_txids);
    auto root_from_branch = compute_root_le_hex(txids[0], branch);
    auto root_from_brute  = brute_root(txids);
    CHECK(root_from_branch == root_from_brute);
}
