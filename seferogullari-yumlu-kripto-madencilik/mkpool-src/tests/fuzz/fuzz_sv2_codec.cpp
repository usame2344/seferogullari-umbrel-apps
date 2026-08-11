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
// File:        fuzz_sv2_codec.cpp
// Description: libFuzzer target for the SV2 Reader primitives themselves.
// Created:     2026-07-15
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool
//
// The message target drives the Reader only in the fixed field order each struct
// uses. This one lets the fuzzer pick the read sequence, reaching length/primitive
// combinations no real message shape produces -- notably the length-prefixed
// reads (STR0_255, B0_32, B0_255, B0_64K, seq0_255_u256) where a length byte
// drives a copy.
//
// Contract: every primitive returns a value fully inside the input, or throws.
// Never a read past size_.

#include "sv2_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>

namespace sv2 = mkpool::sv2;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;

    // First byte is an opcode stream seed; the rest is the buffer being read.
    // Re-using the same bytes as both program and data is deliberate: it lets
    // the fuzzer steer reads toward the boundaries of its own buffer.
    sv2::Reader r(data, size);

    try {
        for (size_t i = 0; i < size; ++i) {
            // Stop once exhausted; further reads would just throw immediately
            // and add no coverage.
            if (r.remaining() == 0) break;

            switch (data[i] % 12) {
                case 0:  (void)r.read_u8();               break;
                case 1:  (void)r.read_bool();             break;
                case 2:  (void)r.read_u16();              break;
                case 3:  (void)r.read_u24();              break;
                case 4:  (void)r.read_u32();              break;
                case 5:  (void)r.read_u64();              break;
                case 6:  (void)r.read_u256();             break;
                case 7:  (void)r.read_f32();              break;
                case 8:  (void)r.read_str0_255();         break;
                case 9:  (void)r.read_b0_32();            break;
                case 10: (void)r.read_b0_255();           break;
                case 11: (void)r.read_b0_64k();           break;
                default: break;
            }
        }

        // Exercise the remaining two accessors that take a length prefix.
        if (r.remaining() > 0) (void)r.read_option_u32();
        if (r.remaining() > 0) (void)r.read_seq0_255_u256();
    } catch (const std::exception&) {
        // Running off the end of the buffer is the designed behaviour.
    }

    // Position must never escape the buffer, throw or not.
    if (r.pos() > size) __builtin_trap();

    return 0;
}
