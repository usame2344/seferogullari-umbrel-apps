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
// File:        test_address.cpp
// Description: Unit tests for address decoding/validation.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include <catch2/catch_test_macros.hpp>
#include "address.hpp"

using mkpool::address::decode;
using mkpool::address::Type;
using mkpool::address::Network;

TEST_CASE("bech32 mainnet P2WPKH", "[address][bech32]") {
    auto d = decode("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4");
    REQUIRE(d.has_value());
    CHECK(d->network == Network::BitcoinMainnet);
    CHECK(d->type    == Type::P2WPKH);
}

TEST_CASE("bech32 testnet P2WPKH", "[address][bech32]") {
    auto d = decode("tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx");
    REQUIRE(d.has_value());
    CHECK(d->network == Network::BitcoinTestnet);
    CHECK(d->type    == Type::P2WPKH);
}

TEST_CASE("bech32m mainnet P2TR", "[address][bech32m]") {
    auto d = decode("bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0");
    REQUIRE(d.has_value());
    CHECK(d->type == Type::P2TR);
}

TEST_CASE("base58check legacy P2PKH", "[address][base58]") {
    auto d = decode("1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa");
    REQUIRE(d.has_value());
    CHECK(d->type    == Type::P2PKH);
    CHECK(d->network == Network::BitcoinMainnet);
}

TEST_CASE("base58check P2SH", "[address][base58]") {
    auto d = decode("3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy");
    REQUIRE(d.has_value());
    CHECK(d->type == Type::P2SH);
}

TEST_CASE("invalid checksum is rejected", "[address][invalid]") {
    CHECK_FALSE(decode("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t5").has_value());
    CHECK_FALSE(decode("1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNb").has_value());
}

TEST_CASE("empty input is rejected", "[address][invalid]") {
    auto d = decode("");
    REQUIRE_FALSE(d.has_value());
}

TEST_CASE("BCH cashaddr decoded", "[address][cashaddr]") {
    auto d = decode("bitcoincash:qpm2qsznhks23z7629mms6s4cwef74vcwvy22gdx6a");
    REQUIRE(d.has_value());
    CHECK((d->type == Type::BchP2PKH || d->type == Type::BchP2SH));
}
