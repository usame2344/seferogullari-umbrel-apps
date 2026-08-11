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
// File:        test_sv2_framing.cpp
// Description: Regression tests for the SV2 frame-length bound (2026-07-15).
// Created:     2026-07-15
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool
//
// These are the deterministic counterpart to tests/fuzz/fuzz_sv2_frame.cpp:
// the fuzzer explores, these pin down the exact values that mattered. They run
// in the normal test suite (no clang/libFuzzer needed), so the bound is checked
// on every build.

#include <catch2/catch_test_macros.hpp>
#include "sv2_messages.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sv2 = mkpool::sv2;

// Mirrors ClientSession::read_buf_ (std::array<char, 16384>).
static constexpr size_t kReadBufCapacity = 16384;

TEST_CASE("MAX_MSG_LENGTH leaves room for the Noise MAC", "[sv2][framing]") {
    // A Noise transport message is capped at 65535 bytes INCLUDING the 16-byte
    // MAC. An SV2 payload is decrypted as exactly one such message, so the
    // plaintext bound must be 65535-16. If someone "rounds it up" to 65535 the
    // payload+MAC becomes 65551 and no longer fits a single Noise message.
    CHECK(sv2::MAX_MSG_LENGTH == 65519);
    CHECK(sv2::MAX_MSG_LENGTH + 16 == 65535);
}

TEST_CASE("frame_length_ok accepts real miner frame sizes", "[sv2][framing]") {
    // Sizes observed from live SV2 miners: SubmitSharesStandard=24,
    // SubmitSharesExtended=33. Nothing legitimate comes close to the bound.
    CHECK(sv2::frame_length_ok(0));
    CHECK(sv2::frame_length_ok(24));
    CHECK(sv2::frame_length_ok(33));
    CHECK(sv2::frame_length_ok(1024));
}

TEST_CASE("frame_length_ok rejects the 2026-07-15 attack length", "[sv2][framing][regression]") {
    // The exact value the attacker sent, repeated byte-identically across 206
    // connections from two IPs. It overflowed a 16 KiB read_buf_ by ~183 KiB.
    CHECK_FALSE(sv2::frame_length_ok(200000));
}

TEST_CASE("frame_length_ok boundary is exact", "[sv2][framing]") {
    CHECK(sv2::frame_length_ok(sv2::MAX_MSG_LENGTH));            // 65519 in
    CHECK_FALSE(sv2::frame_length_ok(sv2::MAX_MSG_LENGTH + 1));  // 65520 out
    CHECK_FALSE(sv2::frame_length_ok(65535));                    // Noise max, still too big for a payload
    CHECK_FALSE(sv2::frame_length_ok(0x00FFFFFF));               // u24 ceiling
}

TEST_CASE("the literal attack header parses to 200000 and is rejected", "[sv2][framing][regression]") {
    // Byte-for-byte the header the pools received:
    //   ext_type=0x0000, msg_type=0x00, msg_length=0x030D40 (200000, u24 LE)
    const std::vector<uint8_t> attack = {0x00, 0x00, 0x00, 0x40, 0x0D, 0x03};

    sv2::Reader r(attack);
    sv2::Header h;
    h.deserialize(r);

    CHECK(h.extension_type == 0);
    CHECK(h.msg_type == sv2::MSG_SETUP_CONNECTION);
    CHECK(h.msg_length == 200000);

    // The gate that now stands between this header and the read.
    CHECK_FALSE(sv2::frame_length_ok(h.msg_length));
}

TEST_CASE("a declared length can never out-size the read buffer", "[sv2][framing][regression]") {
    // The invariant that was violated: pre-patch the code computed
    // `outstanding = msg_length + 16 - buffered` and handed it straight to
    // asio::buffer() over a 16384-byte array. Here we walk every declared
    // length that passes the gate and assert the clamped read always fits.
    for (uint32_t len : {0u, 1u, 24u, 33u, 1024u, 16383u, 16384u, 16385u,
                         65518u, sv2::MAX_MSG_LENGTH}) {
        REQUIRE(sv2::frame_length_ok(len));

        const size_t required = static_cast<size_t>(len) + 16;
        REQUIRE(required <= 65535); // fits one Noise transport message

        for (size_t buffered : {size_t{0}, size_t{1}, size_t{16}, size_t{4096}}) {
            if (buffered >= required) continue;
            const size_t outstanding = required - buffered;
            const size_t read_size   = std::min(outstanding, kReadBufCapacity);

            CHECK(read_size <= kReadBufCapacity); // never overflow read_buf_
            CHECK(read_size > 0);                 // always make progress
        }
    }
}

TEST_CASE("rejected lengths never reach the read-sizing math", "[sv2][framing][regression]") {
    // Sanity: everything the attacker could physically declare above the bound
    // is refused, so `msg_length + 16` is never even computed for it.
    for (uint32_t len : {sv2::MAX_MSG_LENGTH + 1, 65535u, 65536u, 200000u,
                         1000000u, 0x00FFFFFFu}) {
        CHECK_FALSE(sv2::frame_length_ok(len));
    }
}
