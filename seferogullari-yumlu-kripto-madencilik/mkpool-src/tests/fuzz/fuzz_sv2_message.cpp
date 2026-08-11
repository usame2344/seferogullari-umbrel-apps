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
// File:        fuzz_sv2_message.cpp
// Description: libFuzzer target for every client -> server SV2 message parser.
// Created:     2026-07-15
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool
//
// These are the only SV2 structures an unauthenticated peer can hand us, i.e.
// the whole client-reachable parse surface.
//
// Contract: for ANY byte string a parser either succeeds or throws. Never an
// out-of-bounds read, an over-allocation, or memory corruption. Run under
// ASan/UBSan, or a violation passes silently.
//
// Input format: byte 0 selects the parser, the rest is the message body.

#include "sv2_messages.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>

namespace sv2 = mkpool::sv2;

namespace {

// Parse one message body of the selected type. Kept exhaustive on purpose: if a
// new client -> server message gains a deserialize(), add it here, otherwise it
// ships unfuzzed.
void parse_one(uint8_t selector, const uint8_t* body, size_t body_size) {
    sv2::Reader r(body, body_size);

    switch (selector % 9) {
        case 0: { sv2::Header                    m; m.deserialize(r); break; }
        case 1: { sv2::SetupConnection           m; m.deserialize(r); break; }
        case 2: { sv2::OpenStandardMiningChannel m; m.deserialize(r); break; }
        case 3: { sv2::OpenExtendedMiningChannel m; m.deserialize(r); break; }
        case 4: { sv2::UpdateChannel             m; m.deserialize(r); break; }
        case 5: { sv2::CloseChannel              m; m.deserialize(r); break; }
        case 6: { sv2::SubmitSharesStandard      m; m.deserialize(r); break; }
        case 7: { sv2::SubmitSharesExtended      m; m.deserialize(r); break; }
        case 8: { sv2::SetCustomMiningJob        m; m.deserialize(r); break; }
        default: break;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;

    try {
        parse_one(data[0], data + 1, size - 1);
    } catch (const std::exception&) {
        // Expected: truncated/garbage input is rejected by the Reader's bounds
        // checks. A clean throw is a PASS. Only memory unsafety (caught by the
        // sanitizer) or a non-std:: escape is a failure.
    }

    return 0;
}
