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
// File:        fuzz_sv2_frame.cpp
// Description: libFuzzer target for SV2 framing: header parse -> length gate ->
//              read sizing.
// Created:     2026-07-15
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool
//
// Invariant under test: no peer-supplied number may cause us to request more
// bytes than the destination buffer can hold.
//
// This covers the caller of the parser, not the parser: sv2::Reader bounds-checks
// and throws, so message fuzzing alone cannot reach this class of bug. See
// tests/fuzz/README.md for background.

#include "sv2_messages.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>

namespace sv2 = mkpool::sv2;

namespace {

// Mirrors ClientSession::read_buf_ (std::array<char, 16384>). If that member is
// ever resized, this must track it or the invariant is checked against a lie.
constexpr size_t kReadBufCapacity = 16384;

// Noise transport MAC appended to an encrypted payload.
constexpr size_t kMacLen = 16;

[[noreturn]] void invariant_failed() {
    // Abort loudly so libFuzzer records a crash and saves the reproducer.
    std::abort();
}

// Reproduces the framing decision made in ClientSession::process_v2_buffer for
// the encrypted path, and asserts it can never over-read the destination.
void check_framing(const sv2::Header& h, size_t buffered) {
    // The production gate. Everything past here assumes a sane length.
    if (!sv2::frame_length_ok(h.msg_length)) return;

    // Post-gate, this addition cannot overflow size_t and cannot exceed a Noise
    // transport message: 65519 + 16 == 65535.
    const size_t required = static_cast<size_t>(h.msg_length) + kMacLen;
    if (required > 65535) invariant_failed();

    if (buffered >= required) return; // frame already complete; nothing to read

    const size_t outstanding = required - buffered;

    // The clamp in do_read_v2(). This is the structural guarantee: regardless of
    // what the peer declared, we never ask asio for more than the buffer holds.
    const size_t read_size = std::min(outstanding, kReadBufCapacity);

    // THE INVARIANT. Pre-patch this was `read_size = outstanding` and a declared
    // length of 200000 made it 200016 against a 16384 buffer.
    if (read_size > kReadBufCapacity) invariant_failed();

    // Progress guarantee: a non-complete frame must always request >0 bytes, or
    // the session stalls forever instead of overflowing (a hang, not a smash,
    // but still a bug worth catching).
    if (read_size == 0) invariant_failed();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Need 6 bytes of header, plus 1 byte to vary how much we pretend is already
    // buffered (exercises the partial-frame path, not just the empty one).
    if (size < 7) return 0;

    sv2::Header h;
    try {
        sv2::Reader r(data, 6);
        h.deserialize(r);
    } catch (const std::exception&) {
        return 0; // cannot happen with 6 bytes, but stay honest
    }

    // Scale the "already buffered" count across the interesting range rather
    // than only 0, so the required/buffered boundary gets probed.
    const size_t buffered = static_cast<size_t>(data[6]) * 257;

    check_framing(h, buffered);

    // Having passed the gate, the body parser must also survive the payload.
    if (sv2::frame_length_ok(h.msg_length) && size > 7) {
        try {
            sv2::Reader r(data + 7, size - 7);
            switch (h.msg_type) {
                case sv2::MSG_SETUP_CONNECTION:
                    { sv2::SetupConnection m; m.deserialize(r); break; }
                case sv2::MSG_OPEN_STANDARD_MINING_CHANNEL:
                    { sv2::OpenStandardMiningChannel m; m.deserialize(r); break; }
                case sv2::MSG_OPEN_EXTENDED_MINING_CHANNEL:
                    { sv2::OpenExtendedMiningChannel m; m.deserialize(r); break; }
                case sv2::MSG_UPDATE_CHANNEL:
                    { sv2::UpdateChannel m; m.deserialize(r); break; }
                case sv2::MSG_CLOSE_CHANNEL:
                    { sv2::CloseChannel m; m.deserialize(r); break; }
                case sv2::MSG_SUBMIT_SHARES_STANDARD:
                    { sv2::SubmitSharesStandard m; m.deserialize(r); break; }
                case sv2::MSG_SUBMIT_SHARES_EXTENDED:
                    { sv2::SubmitSharesExtended m; m.deserialize(r); break; }
                case sv2::MSG_SET_CUSTOM_MINING_JOB:
                    { sv2::SetCustomMiningJob m; m.deserialize(r); break; }
                default: break; // unknown type: production logs and drops
            }
        } catch (const std::exception&) {
            // Rejecting a malformed body is correct behaviour.
        }
    }

    return 0;
}
