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
// File:        merkle.cpp
// Description: Merkle branch construction for the coinbase/transaction tree.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "merkle.hpp"

#include <openssl/evp.h>

namespace mkpool::merkle {

namespace {
inline std::uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return std::uint8_t(c - '0');
    if (c >= 'a' && c <= 'f') return std::uint8_t(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return std::uint8_t(c - 'A' + 10);
    throw std::runtime_error("bad hex");
}

std::vector<std::uint8_t> from_hex(std::string_view h) {
    if (h.size() % 2) throw std::runtime_error("odd hex length");
    std::vector<std::uint8_t> out;
    out.reserve(h.size()/2);
    for (std::size_t i = 0; i < h.size(); i += 2) {
        out.push_back(std::uint8_t((hex_nibble(h[i]) << 4) | hex_nibble(h[i+1])));
    }
    return out;
}

std::string to_hex(std::span<const std::uint8_t> data) {
    static constexpr char L[] = "0123456789abcdef";
    std::string s;
    s.reserve(data.size()*2);
    for (auto b : data) { s.push_back(L[b>>4]); s.push_back(L[b&0xf]); }
    return s;
}
} // anon

Hash sha256d(std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 32> first{};
    std::array<std::uint8_t, 32> second{};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
    unsigned int len = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, first.data(), &len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("sha256 1 failed");
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, first.data(), first.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, second.data(), &len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("sha256 2 failed");
    }
    EVP_MD_CTX_free(ctx);
    return second;
}

std::vector<std::string>
build_branch_le_hex(std::span<const std::string> txids_le_hex) {
    // The merkle branch for the coinbase (leaf 0) consists of the sibling at
    // each level of the merkle tree, starting from the level above the leaves.
    // We compute it by iteratively halving the layer while tracking the
    // sibling of the "current path" node, which is always at index 0 -> 0 -> 0
    // because the coinbase sits in slot 0.
    //
    // Algorithm (Bitcoin Core / btcd convention):
    //   level = [ <coinbase placeholder>, tx1, tx2, ..., txN ]
    //   branch = []
    //   while level.size() > 1:
    //       if len(level) odd: append last element
    //       branch.push(level[1])           # sibling of slot 0
    //       new = []
    //       for i in 0,2,4,...: new.push(sha256d(level[i] || level[i+1]))
    //       level = new
    //
    // We never actually need the coinbase txid value because it always sits at
    // slot 0 and is replaced by the merkle root computation later.

    std::vector<Hash> level;
    level.reserve(txids_le_hex.size() + 1);
    // Placeholder for coinbase at slot 0 (value doesn't influence branch contents).
    level.emplace_back();
    for (const auto& h : txids_le_hex) {
        auto b = from_hex(h);
        if (b.size() != 32) throw std::runtime_error("merkle: txid not 32 bytes");
        Hash hh{};
        std::memcpy(hh.data(), b.data(), 32);
        level.push_back(hh);
    }

    std::vector<std::string> branch;
    if (level.size() == 1) return branch; // only coinbase: branch is empty

    while (level.size() > 1) {
        if (level.size() % 2) level.push_back(level.back());
        // Sibling of slot 0 is slot 1.
        branch.push_back(to_hex(level[1]));

        std::vector<Hash> next;
        next.reserve(level.size() / 2);
        std::array<std::uint8_t, 64> buf{};
        for (std::size_t i = 0; i < level.size(); i += 2) {
            // Combine in internal byte order. Stratum LE bytes ARE the internal
            // order used by Bitcoin's tree hashing (Bitcoin reverses for display
            // only).
            std::memcpy(buf.data(),      level[i].data(),   32);
            std::memcpy(buf.data() + 32, level[i+1].data(), 32);
            next.push_back(sha256d(buf));
        }
        level = std::move(next);
    }
    return branch;
}

std::string compute_root_le_hex(std::string_view coinbase_txid_le_hex,
                                std::span<const std::string> branch_le_hex) {
    auto cur = from_hex(coinbase_txid_le_hex);
    if (cur.size() != 32) throw std::runtime_error("coinbase txid length");
    std::array<std::uint8_t, 64> buf{};
    for (const auto& sib_hex : branch_le_hex) {
        auto sib = from_hex(sib_hex);
        if (sib.size() != 32) throw std::runtime_error("branch element length");
        std::memcpy(buf.data(),      cur.data(), 32);
        std::memcpy(buf.data() + 32, sib.data(), 32);
        auto h = sha256d(buf);
        cur.assign(h.begin(), h.end());
    }
    return to_hex(cur);
}

} // namespace mkpool::merkle
