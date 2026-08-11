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
// File:        address.hpp
// Description: Address decoding/validation and scriptPubKey builder interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mkpool::address {

    enum class Network : std::uint8_t {
        BitcoinMainnet,
        BitcoinTestnet,
        BitcoinSignet,
        BitcoinRegtest,
        BchMainnet,
        BchTestnet,
        BchRegtest,
    };

    enum class Type : std::uint8_t {
        P2PKH,    // 20-byte hash
        P2SH,     // 20-byte hash
        P2WPKH,   // 20-byte witness program
        P2WSH,    // 32-byte witness program
        P2TR,     // 32-byte witness program (bech32m, v1)
        BchP2PKH, // CashAddr, 20-byte hash
        BchP2SH,  // CashAddr, 20-byte hash
    };

    enum class Error : std::uint8_t {
        Empty,
        UnknownPrefix,
        BadLength,
        BadChecksum,
        BadCharacter,
        UnsupportedVersion,
        UnsupportedNetwork,
        // An exception escaped the decoder internals. Indicates a bug here, not
        // a bad address.
        Internal,
    };

    struct Decoded {
        Type    type;
        Network network;
        std::vector<std::uint8_t> program; // 20 or 32 bytes
    };

    // Auto-detect format (CashAddr / bech32 / bech32m / base58check) and decode.
    [[nodiscard]] std::expected<Decoded, Error> decode(std::string_view addr) noexcept;

    // Returns the scriptPubKey *hex* string suitable for direct concatenation
    // into a serialized transaction output.
    [[nodiscard]] std::string script_pubkey_hex(const Decoded& d);

    // Convenience: decode + build scriptPubKey in one call.
    [[nodiscard]] std::expected<std::string, Error>
        decode_to_script_hex(std::string_view addr);

    // Utility: returns the typical output size in vbytes for an address.
    [[nodiscard]] std::size_t output_vbytes(Type t) noexcept;

    // For tests / diagnostics.
    [[nodiscard]] std::string_view error_to_string(Error e) noexcept;

} // namespace mkpool::address
