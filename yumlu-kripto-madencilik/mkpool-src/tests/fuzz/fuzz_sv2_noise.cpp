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
// File:        fuzz_sv2_noise.cpp
// Description: libFuzzer target for the SV2 Noise handshake and AEAD transport.
// Created:     2026-07-15
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool
//
// The first code to touch bytes from an unauthenticated peer on an SV2 port: it
// runs before the frame parser and before every other guard. Noise_NX does not
// authenticate the initiator, and EllSwift maps any 64-byte string to a valid
// curve point, so ANY random bytes complete this handshake -- by design. That
// makes everything below reachable by anyone who can open a TCP connection.
//
// Contracts under test:
//   1. process_act1() + generate_act2() survive any 64-byte "public key".
//   2. Act 2 is always exactly 234 bytes; client_session assumes that framing.
//   3. decrypt() writes at most len-16 bytes and reads nothing past ciphertext+len.
//   4. decrypt() rejects len < 16 rather than underflowing to a huge length.

#include "sv2_noise.hpp"

#include <sodium.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

using mkpool::sv2::NoiseState;

namespace {

// Any valid 32-byte hex key works; the authority key signs the cert in Act 2 and
// is not attacker-reachable. Fixed so runs are reproducible.
const std::string kAuthorityKey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

bool sodium_ready() {
    static const bool ok = (sodium_init() >= 0);
    return ok;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // 64 bytes of "client ellswift pubkey", then whatever is left is treated as
    // transport ciphertext.
    if (size < 64) return 0;
    if (size > 70000) return 0; // past a Noise message there is nothing to learn
    if (!sodium_ready()) return 0;

    NoiseState st;
    if (!st.initialize(kAuthorityKey)) return 0;

    // Contract 1a: the attacker's 64 bytes go in raw. process_act1 memcpy's them
    // into a fixed 64-byte member and mixes them into the hash state.
    if (!st.process_act1(data)) return 0;

    // Contract 1b: generate_act2 does EC math on those attacker bytes. It throws
    // on ellswift failure -- client_session catches that, so a throw here is a
    // handled error path, not a crash.
    std::vector<uint8_t> act2;
    try {
        act2 = st.generate_act2();
    } catch (const std::exception&) {
        return 0;
    }

    // Contract 2: client_session writes act2 assuming a fixed 234-byte frame.
    // A different size would desync every SV2 client.
    if (act2.size() != 234) __builtin_trap();
    if (!st.is_handshake_complete()) __builtin_trap();

    const uint8_t* ct     = data + 64;
    const size_t   ct_len = size - 64;

    // Contract 4: the length guard. Anything under a MAC must be refused, not
    // turned into a huge len-16 via unsigned underflow.
    {
        uint8_t sink[1] = {0};
        for (size_t bad : {size_t{0}, size_t{1}, size_t{15}}) {
            if (st.decrypt(ct, bad, sink)) __builtin_trap();
        }
    }

    // Contract 3: decrypt writes exactly len-16 plaintext bytes. Size the output
    // to precisely that, so ASan catches a single byte of overrun. The AEAD will
    // almost always reject the tag -- the point is that it must reject SAFELY,
    // having touched nothing outside its buffers.
    if (ct_len >= 16) {
        const size_t plain_len = ct_len - 16;
        std::vector<uint8_t> out(std::max<size_t>(plain_len, 1));
        (void)st.decrypt(ct, ct_len, out.data());
    }

    // Drive the header-shaped call client_session actually makes: exactly 22
    // bytes in, exactly 6 out. This is the call that fronted the whole incident.
    if (ct_len >= 22) {
        uint8_t header[6] = {0};
        (void)st.decrypt(ct, 22, header);
    }

    // Repeated calls advance rx_nonce_ only on success; run a few to exercise the
    // counter path and any state carried between frames.
    if (ct_len >= 32) {
        uint8_t out[16] = {0};
        for (int i = 0; i < 3; ++i) (void)st.decrypt(ct, 32, out);
    }

    return 0;
}
