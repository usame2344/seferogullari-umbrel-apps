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
// File:        fuzz_address.cpp
// Description: libFuzzer target for payout-address decoding (mining.authorize).
// Created:     2026-07-15
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool
//
// The payout address is chosen by the peer (mining.authorize) and fed to the
// base58 / bech32 / bech32m / cashaddr decoders.
//
// Contracts under test:
//  1. decode() is noexcept -- an escaping exception is std::terminate, not an
//     error return.
//  2. A successful decode yields a 20- or 32-byte program matching its Type.
//     A wrong-size program builds a malformed scriptPubKey, i.e. a block that
//     pays nobody.

#include "address.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace addr = mkpool::address;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Cap input: past a few KiB there is nothing new to learn, and letting the
    // fuzzer grow multi-MiB strings just burns the time budget.
    if (size > 4096) return 0;

    const std::string_view sv(reinterpret_cast<const char*>(data), size);

    // Contract 1: must not throw (noexcept => terminate if it does).
    const auto d = addr::decode(sv);

    if (d.has_value()) {
        // Contract 2: a successful decode must yield a usable witness program /
        // hash. Everything downstream (script_pubkey_hex, coinbase assembly)
        // assumes this.
        const std::size_t n = d->program.size();
        if (n != 20 && n != 32) __builtin_trap();

        // Type and program size must agree; a P2TR with a 20-byte program would
        // silently build the wrong script.
        switch (d->type) {
            case addr::Type::P2PKH:
            case addr::Type::P2SH:
            case addr::Type::P2WPKH:
            case addr::Type::BchP2PKH:
            case addr::Type::BchP2SH:
                if (n != 20) __builtin_trap();
                break;
            case addr::Type::P2WSH:
            case addr::Type::P2TR:
                if (n != 32) __builtin_trap();
                break;
        }

        // Decoding is a pure function of the string: same input, same output.
        const auto again = addr::decode(sv);
        if (!again.has_value() || again->program != d->program) __builtin_trap();

        // script_pubkey_hex() is NOT noexcept, so guard it; it must still never
        // corrupt memory on any program a decode actually produced.
        try {
            const std::string script = addr::script_pubkey_hex(*d);
            if (script.empty()) __builtin_trap(); // a decoded address always has a script
        } catch (const std::exception&) {
            // Tolerated: an exception here is an error path, not memory unsafety.
        }
    }

    return 0;
}
