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
// File:        equihash.hpp
// Description: Equihash (200,9) solution verifier for Zcash shares.
// Created:     2026-05-28
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <sodium.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <memory>
#include <string>

namespace mkpool::equihash {

// Portable endian helpers
inline uint32_t to_le32(uint32_t val) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(val);
#else
    return val;
#endif
}

inline uint32_t to_be32(uint32_t val) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return val;
#else
    return __builtin_bswap32(val);
#endif
}

inline uint32_t from_be32(uint32_t val) {
    return to_be32(val);
}

inline void ExpandArray(const unsigned char* in, size_t in_len,
                        unsigned char* out, size_t out_len,
                        size_t bit_len, size_t byte_pad = 0)
{
    assert(bit_len >= 8);
    size_t out_width = (bit_len + 7) / 8 + byte_pad;
    assert(out_len == 8 * out_width * in_len / bit_len);

    uint32_t bit_len_mask = ((uint32_t)1 << bit_len) - 1;
    size_t acc_bits = 0;
    uint32_t acc_value = 0;
    size_t j = 0;

    for (size_t i = 0; i < in_len; i++) {
        acc_value = (acc_value << 8) | in[i];
        acc_bits += 8;

        if (acc_bits >= bit_len) {
            acc_bits -= bit_len;
            for (size_t x = 0; x < byte_pad; x++) {
                out[j + x] = 0;
            }
            for (size_t x = byte_pad; x < out_width; x++) {
                out[j + x] = (
                    acc_value >> (acc_bits + (8 * (out_width - x - 1)))
                ) & (
                    (bit_len_mask >> (8 * (out_width - x - 1))) & 0xFF
                );
            }
            j += out_width;
        }
    }
}

inline std::vector<uint32_t> GetIndicesFromMinimal(const std::vector<unsigned char>& soln, size_t cBitLen) {
    size_t lenIndices = 8 * sizeof(uint32_t) * soln.size() / (cBitLen + 1);
    size_t bytePad = sizeof(uint32_t) - ((cBitLen + 1) + 7) / 8;
    std::vector<unsigned char> array(lenIndices);
    ExpandArray(soln.data(), soln.size(), array.data(), lenIndices, cBitLen + 1, bytePad);

    std::vector<uint32_t> ret;
    ret.reserve(lenIndices / 4);
    for (size_t i = 0; i < lenIndices; i += 4) {
        uint32_t bei;
        std::memcpy(&bei, array.data() + i, 4);
        ret.push_back(from_be32(bei));
    }
    return ret;
}

// Compact row structure for Wagner verification
struct StepRow {
    uint8_t hash[2054]; // FinalFullWidth = 6 + 4 * 512 = 2054 bytes max

    StepRow() {
        std::memset(hash, 0, sizeof(hash));
    }

    void Init(const unsigned char* hashIn, size_t hInLen, size_t hLen, size_t cBitLen, uint32_t index) {
        ExpandArray(hashIn, hInLen, hash, hLen, cBitLen);
        uint32_t bei = to_be32(index);
        std::memcpy(hash + hLen, &bei, 4);
    }

    void InitCollide(const StepRow& a, const StepRow& b, size_t len, size_t lenIndices, size_t trim) {
        for (size_t i = trim; i < len; i++) {
            hash[i - trim] = a.hash[i] ^ b.hash[i];
        }
        if (std::memcmp(a.hash + len, b.hash + len, lenIndices) < 0) {
            std::copy(a.hash + len, a.hash + len + lenIndices, hash + len - trim);
            std::copy(b.hash + len, b.hash + len + lenIndices, hash + len - trim + lenIndices);
        } else {
            std::copy(b.hash + len, b.hash + len + lenIndices, hash + len - trim);
            std::copy(a.hash + len, a.hash + len + lenIndices, hash + len - trim + lenIndices);
        }
    }

    bool IsZero(size_t len) const {
        for (size_t i = 0; i < len; i++) {
            if (hash[i] != 0) return false;
        }
        return true;
    }
};

inline bool DistinctIndices(const StepRow& a, const StepRow& b, size_t len, size_t lenIndices) {
    for (size_t i = 0; i < lenIndices; i += 4) {
        for (size_t j = 0; j < lenIndices; j += 4) {
            if (std::memcmp(a.hash + len + i, b.hash + len + j, 4) == 0) {
                return false;
            }
        }
    }
    return true;
}

inline bool verify_200_9(const std::vector<unsigned char>& header, const std::vector<unsigned char>& soln) {
    constexpr unsigned int N = 200;
    constexpr unsigned int K = 9;
    constexpr size_t SolutionWidth = 1344;
    constexpr size_t CollisionBitLength = 20; // N / (K + 1)
    constexpr size_t CollisionByteLength = 3;
    constexpr size_t HashLength = 30; // (K + 1) * 3
    constexpr size_t IndicesPerHashOutput = 2; // 512 / 200
    constexpr size_t HashOutput = 50;

    if (soln.size() != SolutionWidth) {
        return false;
    }

    // Initialize libsodium BLAKE2b personalized state
    crypto_generichash_blake2b_state base_state;
    unsigned char personalization[16] = {};
    std::memcpy(personalization, "ZcashPoW", 8);
    uint32_t le_N = to_le32(N);
    uint32_t le_K = to_le32(K);
    std::memcpy(personalization + 8, &le_N, 4);
    std::memcpy(personalization + 12, &le_K, 4);

    if (crypto_generichash_blake2b_init_salt_personal(
            &base_state,
            NULL, 0, // No key
            HashOutput,
            NULL,    // No salt
            personalization) != 0) {
        return false;
    }

    // Update state with block header
    crypto_generichash_blake2b_update(&base_state, header.data(), header.size());

    // Unpack solution indices
    std::vector<uint32_t> indices = GetIndicesFromMinimal(soln, CollisionBitLength);
    if (indices.size() != (1 << K)) {
        return false;
    }

    // Populate initial rows
    std::vector<StepRow> X((1 << K));
    unsigned char tmpHash[HashOutput];

    for (size_t i = 0; i < indices.size(); ++i) {
        uint32_t idx = indices[i];
        crypto_generichash_blake2b_state state = base_state;
        uint32_t lei = to_le32(idx / IndicesPerHashOutput);
        crypto_generichash_blake2b_update(&state, (const unsigned char*)&lei, 4);
        crypto_generichash_blake2b_final(&state, tmpHash, HashOutput);

        size_t offset = (idx % IndicesPerHashOutput) * 25;
        X[i].Init(tmpHash + offset, 25, HashLength, CollisionBitLength, idx);
    }

    // Solve binary tree rounds
    size_t hashLen = HashLength;
    size_t lenIndices = 4; // sizeof(uint32_t)

    while (X.size() > 1) {
        std::vector<StepRow> Xc;
        Xc.reserve(X.size() / 2);

        for (size_t i = 0; i < X.size(); i += 2) {
            if (std::memcmp(X[i].hash, X[i + 1].hash, CollisionByteLength) != 0) {
                return false; // Collision failed
            }
            if (std::memcmp(X[i + 1].hash + hashLen, X[i].hash + hashLen, lenIndices) < 0) {
                return false; // Order rule violation (prevents amortization bypass)
            }
            if (!DistinctIndices(X[i], X[i + 1], hashLen, lenIndices)) {
                return false; // Duplicate index found (avoids double-spend/fake solutions)
            }
            StepRow row;
            row.InitCollide(X[i], X[i + 1], hashLen, lenIndices, CollisionByteLength);
            Xc.push_back(row);
        }
        X = std::move(Xc);
        hashLen -= CollisionByteLength;
        lenIndices *= 2;
    }

    assert(X.size() == 1);
    return X[0].IsZero(hashLen);
}

} // namespace mkpool::equihash
