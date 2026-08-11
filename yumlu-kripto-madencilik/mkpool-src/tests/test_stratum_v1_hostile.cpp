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
// File:        test_stratum_v1_hostile.cpp
// Description: Adversarial input tests for the Stratum V1 surface.
// Created:     2026-07-15
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool
//
// The existing V1 tests cover what a well-behaved miner sends; these cover what
// an attacker sends.
//
// Two functions here are noexcept AND fed attacker-controlled data:
//
//   stratum::negotiate_configure()  <- arbitrary JSON from mining.configure
//   address::decode()               <- arbitrary string from mining.authorize
//
// Both now catch internally, but note what that means for these tests: a
// noexcept violation is std::terminate, so "the test ran to completion" IS the
// assertion. A regression would not show up as a red CHECK -- it would kill the
// test binary outright.

#include <catch2/catch_test_macros.hpp>

#include "stratum_protocol.hpp"
#include "address.hpp"
#include "utils.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <string>

using nlohmann::json;
using mkpool::stratum::negotiate_configure;
using mkpool::stratum::validate_version;
using mkpool::address::decode;

// BIP320 mask the pool actually advertises.
static constexpr std::uint32_t kPoolMask = 0x1fffe000u;

// ---------------------------------------------------------------------------
// mining.configure - negotiate_configure(json, mask) noexcept
// ---------------------------------------------------------------------------

TEST_CASE("configure: params of the wrong shape are refused, not crashed", "[stratum][v1][hostile]") {
    // Every one of these reaches negotiate_configure in production: process_line
    // only guarantees the frame parsed as JSON and has a string "method". The
    // params can be literally anything.
    for (const json& p : {
             json(nullptr),
             json(true),
             json(42),
             json(3.14),
             json("a string"),
             json::object(),
             json::array(),                       // empty array
             json::array({json::array()}),        // 1 element, needs 2
             json::array({1, 2}),                 // right size, wrong types
             json::array({json::array(), 5}),     // extra not an object
             json::array({5, json::object()}),    // features not an array
         }) {
        auto r = negotiate_configure(p, kPoolMask);
        CHECK_FALSE(r.version_rolling);
        CHECK_FALSE(r.version_rolling_requested);
        CHECK(r.version_mask == 0);
    }
}

TEST_CASE("configure: non-string entries in the feature list", "[stratum][v1][hostile]") {
    // has_feature() iterates and compares; a non-string entry must be skipped
    // rather than get_ref'd (which would throw inside a noexcept function).
    json params = json::array({
        json::array({1, nullptr, true, json::object(), json::array({"nested"}), "version-rolling"}),
        json::object({{"version-rolling.mask", "1fffe000"}})
    });
    auto r = negotiate_configure(params, kPoolMask);
    // The real feature is still found despite the junk around it.
    CHECK(r.version_rolling_requested);
    CHECK(r.version_rolling);
    CHECK(r.version_mask == kPoolMask);
}

TEST_CASE("configure: version-rolling.mask of every wrong type", "[stratum][v1][hostile]") {
    for (const json& mask : {json(123), json(1.5), json(true), json(nullptr),
                             json::object(), json::array({"1fffe000"})}) {
        json params = json::array({
            json::array({"version-rolling"}),
            json::object({{"version-rolling.mask", mask}})
        });
        auto r = negotiate_configure(params, kPoolMask);
        // Non-string mask => miner_mask stays 0xffffffff => intersection is the
        // pool mask. Requested, and granted at our own mask: never a crash.
        CHECK(r.version_rolling_requested);
        CHECK(r.version_mask == kPoolMask);
    }
}

TEST_CASE("configure: version-rolling.mask garbage strings", "[stratum][v1][hostile]") {
    struct Case { const char* mask; bool expect_rolling; };
    const Case cases[] = {
        {"",                        false}, // stoul throws -> mask 0 -> no intersection
        {"zzzz",                    false}, // not hex at all
        {"0x1fffe000",              true},  // stoul(base16) tolerates the 0x prefix
        {"00000000",                false}, // explicit zero -> empty intersection
        {"ffffffffffffffffffffff",  false}, // out_of_range -> caught -> mask 0
        {"-1",                      true},  // stoul accepts a sign; wraps, still intersects
        {"1fffe000\0hidden",        true},  // embedded NUL: stoul stops at it
    };
    for (const auto& c : cases) {
        json params = json::array({
            json::array({"version-rolling"}),
            json::object({{"version-rolling.mask", std::string(c.mask)}})
        });
        auto r = negotiate_configure(params, kPoolMask);
        CHECK(r.version_rolling_requested);
        CHECK(r.version_rolling == c.expect_rolling);
        // Whatever the miner claimed, we never grant a bit outside the pool mask.
        CHECK((r.version_mask & ~kPoolMask) == 0);
    }
}

TEST_CASE("configure: granted mask never escapes the pool mask", "[stratum][v1][hostile]") {
    // The invariant that matters: a miner must never negotiate rolling rights
    // over bits we did not offer, no matter what it asks for.
    for (const char* mask : {"ffffffff", "1fffe000", "20000000", "7fffffff",
                             "80000000", "00002000", "ffff0000"}) {
        json params = json::array({
            json::array({"version-rolling"}),
            json::object({{"version-rolling.mask", std::string(mask)}})
        });
        auto r = negotiate_configure(params, kPoolMask);
        CHECK((r.version_mask & ~kPoolMask) == 0);
    }
}

TEST_CASE("configure: minimum-difficulty.value of hostile types and magnitudes", "[stratum][v1][hostile]") {
    // This value is miner-supplied and feeds difficulty handling. Wrong types
    // must be ignored outright.
    for (const json& v : {json("1000"), json(nullptr), json(true),
                          json::object(), json::array({1})}) {
        json params = json::array({
            json::array({"minimum-difficulty"}),
            json::object({{"minimum-difficulty.value", v}})
        });
        auto r = negotiate_configure(params, kPoolMask);
        CHECK(r.minimum_difficulty);
        CHECK(r.suggested_min_difficulty == 0.0); // non-number => untouched
    }

    // Numeric extremes are accepted by the parser (is_number passes). Pinning
    // this documents that sanity-clamping is the CALLER's job -- negotiate_
    // configure reports what was asked, it does not police it.
    struct Case { json v; double expect; };
    const Case nums[] = {
        {json(0),          0.0},
        {json(-1),        -1.0},
        {json(-1e308), -1e308},
        {json(1e308),   1e308},
    };
    for (const auto& c : nums) {
        json params = json::array({
            json::array({"minimum-difficulty"}),
            json::object({{"minimum-difficulty.value", c.v}})
        });
        auto r = negotiate_configure(params, kPoolMask);
        CHECK(r.suggested_min_difficulty == c.expect);
    }
}

TEST_CASE("configure: absent extra keys", "[stratum][v1][hostile]") {
    // Feature requested but its parameter object is empty.
    json params = json::array({
        json::array({"version-rolling", "minimum-difficulty", "subscribe-extranonce"}),
        json::object()
    });
    auto r = negotiate_configure(params, kPoolMask);
    CHECK(r.version_rolling_requested);
    CHECK(r.minimum_difficulty);
    CHECK(r.subscribe_extranonce);
    CHECK(r.suggested_min_difficulty == 0.0);
}

TEST_CASE("configure: oversized and adversarial feature lists", "[stratum][v1][hostile]") {
    // A miner can name as many features as it likes; has_feature() is a linear
    // scan, so this is also the shape of a CPU-burn attempt.
    json features = json::array();
    for (int i = 0; i < 5000; ++i) features.push_back("junk-feature-" + std::to_string(i));
    features.push_back("version-rolling");

    json params = json::array({features, json::object({{"version-rolling.mask", "1fffe000"}})});
    auto r = negotiate_configure(params, kPoolMask);
    CHECK(r.version_rolling);
}

TEST_CASE("configure: control characters and unicode in feature names", "[stratum][v1][hostile]") {
    json params = json::array({
        json::array({std::string("ver\x01sion\x1f-rolling"), "\xE2\x98\xA0", std::string("a\0b", 3)}),
        json::object({{"version-rolling.mask", "1fffe000"}})
    });
    auto r = negotiate_configure(params, kPoolMask);
    // None of those match a real feature name.
    CHECK_FALSE(r.version_rolling_requested);
    CHECK_FALSE(r.version_rolling);
}

TEST_CASE("configure: pool mask of zero grants nothing", "[stratum][v1][hostile]") {
    json params = json::array({
        json::array({"version-rolling"}),
        json::object({{"version-rolling.mask", "ffffffff"}})
    });
    auto r = negotiate_configure(params, 0u);
    CHECK(r.version_rolling_requested);
    CHECK_FALSE(r.version_rolling);
    CHECK(r.version_mask == 0);
}

// ---------------------------------------------------------------------------
// validate_version - the anti-cheat gate on submitted block versions
// ---------------------------------------------------------------------------

TEST_CASE("validate_version: only masked bits may differ", "[stratum][v1][hostile]") {
    const std::uint32_t tmpl = 0x20000000u;

    // Rolling disabled: the version must match the template exactly.
    CHECK(validate_version(tmpl, tmpl, 0));
    CHECK_FALSE(validate_version(tmpl | 0x00002000u, tmpl, 0));

    // Rolling enabled: bits inside the mask are the miner's to change...
    CHECK(validate_version(tmpl | 0x00002000u, tmpl, kPoolMask));
    CHECK(validate_version(tmpl | kPoolMask,   tmpl, kPoolMask));

    // ...and bits outside it are not. This is the check that stops a miner
    // mining a different chain's version on our template.
    CHECK_FALSE(validate_version(tmpl | 0x80000000u, tmpl, kPoolMask));
    CHECK_FALSE(validate_version(0u,                 tmpl, kPoolMask));
    CHECK_FALSE(validate_version(0xffffffffu,        tmpl, kPoolMask));
}

TEST_CASE("validate_version: extremes", "[stratum][v1][hostile]") {
    CHECK(validate_version(0xffffffffu, 0x00000000u, 0xffffffffu)); // everything rollable
    CHECK(validate_version(0u, 0u, 0u));
    CHECK_FALSE(validate_version(1u, 0u, 0u));
}

// ---------------------------------------------------------------------------
// mining.submit field decoding - hex_to_bytes / valid_hex
// ---------------------------------------------------------------------------
// nonce, ntime and extranonce2 arrive as miner-supplied hex strings.

TEST_CASE("hex: malformed input throws rather than reading garbage", "[stratum][v1][hostile]") {
    using mkpool::utils::hex_to_bytes;

    CHECK_THROWS(hex_to_bytes("abc"));        // odd length
    CHECK_THROWS(hex_to_bytes("zz"));         // non-hex
    CHECK_THROWS(hex_to_bytes("ab cd"));      // space is not hex
    CHECK_THROWS(hex_to_bytes("0xffff"));     // 'x' is not hex
    CHECK_THROWS(hex_to_bytes("ffff\n"));     // trailing newline survived the split

    // Embedded NUL, with an explicit length so the view actually carries it.
    // Note the trap: hex_to_bytes("ab\0cd") built from a C literal silently
    // becomes "ab" (2 chars, even, valid) and does NOT throw -- the NUL
    // terminates the string_view. Only a length-carrying view sees the NUL.
    CHECK_THROWS(hex_to_bytes(std::string_view("ab\0c", 4)));  // even length, NUL is not hex
    CHECK(hex_to_bytes("ab\0cd").size() == 1);                 // truncated at NUL, no throw

    CHECK(hex_to_bytes("").empty());
    CHECK(hex_to_bytes("00ff").size() == 2);
    CHECK(hex_to_bytes("00FF") == hex_to_bytes("00ff")); // case-insensitive
}

TEST_CASE("hex: throwing is safe only because process_line catches", "[stratum][v1][hostile]") {
    // Documents a real coupling: hex_to_bytes throws, and the ONLY thing that
    // stops a bad mining.submit from taking the pool down is the try/catch
    // around the handler dispatch in ClientSession::process_line. asio rethrows
    // out of handlers. If that catch is ever narrowed, this comment is the
    // reason it must not be.
    using mkpool::utils::hex_to_bytes;
    bool caught = false;
    try { (void)hex_to_bytes("nonsense!"); } catch (const std::exception&) { caught = true; }
    CHECK(caught);
}

TEST_CASE("valid_hex does NOT imply hex_to_bytes will succeed", "[stratum][v1][hostile]") {
    using mkpool::utils::valid_hex;
    using mkpool::utils::hex_to_bytes;

    // Sharp edge worth pinning: valid_hex only checks the ALPHABET, not that the
    // length is even. So `if (valid_hex(s)) hex_to_bytes(s);` still throws on an
    // odd-length string. Any new caller must check both.
    CHECK(valid_hex("abc"));
    CHECK_THROWS(hex_to_bytes("abc"));

    CHECK_FALSE(valid_hex(""));      // empty is rejected by valid_hex...
    CHECK(hex_to_bytes("").empty()); // ...but accepted by hex_to_bytes

    CHECK_FALSE(valid_hex("ab cd"));
    CHECK_FALSE(valid_hex("0x00"));
    CHECK(valid_hex("00FFaa99"));
}

// ---------------------------------------------------------------------------
// mining.authorize - address::decode(string_view) noexcept
// ---------------------------------------------------------------------------
// The payout address is a raw attacker-controlled string handed to base58 /
// bech32 / cashaddr decoders. Historically the richest source of memory-safety
// bugs in this whole codebase's problem domain.

TEST_CASE("address: hostile strings are rejected without throwing", "[address][v1][hostile]") {
    // decode() is noexcept: reaching the end of this test at all proves none of
    // these terminated the process.
    const std::string cases[] = {
        "",
        " ",
        "\n",
        "\t\r\n",
        std::string("\0", 1),
        std::string("bc1q\0suffix", 10),        // embedded NUL mid-address
        "bc1",                                   // prefix only
        "bc1q",                                  // hrp + separator, no data
        "1",                                     // single base58 char
        "3",
        "bitcoincash:",                          // cashaddr prefix, no payload
        "bitcoincash:!!!!",
        "::::::",
        "bc1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq",
        "Bc1Qw508D6Qejxtdg4Y5R3Zarvary0C5Xw7Kv8F3T4", // mixed case: invalid per BIP173
        "0000000000000000000000000000000000",
        "OIl0",                                  // base58-excluded characters
        "\xff\xfe\xfd\xfc",                      // invalid UTF-8
        "\xE2\x98\xA0\xE2\x98\xA0",              // valid UTF-8, not an address
        "../../../etc/passwd",
        "'; DROP TABLE miners; --",
        "$(reboot)",
        "{{7*7}}",
        "<script>alert(1)</script>",
        "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4extra", // valid then junk
        "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfN",     // truncated P2PKH
        "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNaa",   // extended P2PKH
    };

    for (const auto& c : cases) {
        auto d = decode(c);
        CHECK_FALSE(d.has_value());
    }
}

TEST_CASE("address: bech32 case rules follow BIP173", "[address][v1][hostile]") {
    // BIP173 is specific: an address must be ENTIRELY lowercase or ENTIRELY
    // uppercase. Uppercase is valid and some hardware wallets/QR encoders emit
    // it, so rejecting it would lock real miners out. Mixed case is invalid
    // because it breaks the checksum's case-folding.
    //
    // Pinned because "reject anything that looks weird" is the tempting wrong
    // fix here, and it would silently break payouts for uppercase submitters.
    CHECK(decode("BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4").has_value());
    CHECK(decode("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4").has_value());
    CHECK_FALSE(decode("Bc1Qw508D6Qejxtdg4Y5R3Zarvary0C5Xw7Kv8F3T4").has_value());

    // Upper and lower must decode to the SAME program, or we would pay a
    // different address depending on how the miner happened to type it.
    auto lo = decode("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4");
    auto up = decode("BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4");
    REQUIRE(lo.has_value());
    REQUIRE(up.has_value());
    CHECK(lo->program == up->program);
    CHECK(lo->type    == up->type);
    CHECK(lo->network == up->network);
}

TEST_CASE("address: over-long inputs are rejected at the door", "[address][v1][hostile]") {
    // decode() bounds its input at 128 before doing any work. That matters
    // because base58_decode is O(n^2): measured on this machine, 8 KB takes 19ms
    // and 1 MB takes ~295 SECONDS. Callers do bound it today (V1 authorize at
    // 128, SV2 user_identity at 255 by wire format), so this is defence in depth
    // rather than a live hole -- but the guard is what keeps the quadratic
    // unreachable regardless of who calls next.
    //
    // Sizes stay small on purpose: an earlier version of this test used 100 KB
    // inputs and took over 12 minutes under Debug+ASan, testing a string no
    // caller can produce.
    for (std::size_t n : {std::size_t{129}, std::size_t{256}, std::size_t{1024}}) {
        CHECK_FALSE(decode(std::string(n, 'q')).has_value());
        CHECK_FALSE(decode("bc1q" + std::string(n, 'q')).has_value());
        CHECK_FALSE(decode("bitcoincash:q" + std::string(n, 'q')).has_value());
        CHECK_FALSE(decode(std::string(n, '1')).has_value());
    }

    // The bound is exact, and sits above every real encoding.
    CHECK_FALSE(decode(std::string(129, '1')).has_value());
    CHECK(decode("bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0").has_value()); // 62
    CHECK(decode("bitcoincash:qpm2qsznhks23z7629mms6s4cwef74vcwvy22gdx6a").has_value());        // 54
}

TEST_CASE("address: single-character mutations of a valid address are rejected", "[address][v1][hostile]") {
    // Checksums exist precisely to catch this; prove ours actually do. A pool
    // that accepts a corrupted address pays someone else's wallet.
    const std::string good = "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";
    REQUIRE(decode(good).has_value());

    int rejected = 0, total = 0;
    for (std::size_t i = 4; i < good.size(); ++i) { // skip the "bc1q" prefix
        std::string m = good;
        m[i] = (m[i] == 'q') ? 'p' : 'q';
        if (m == good) continue;
        ++total;
        if (!decode(m).has_value()) ++rejected;
    }
    CHECK(total > 0);
    CHECK(rejected == total); // every single-char mutation must fail the checksum
}

TEST_CASE("address: valid addresses still decode", "[address][v1]") {
    // Guard against a hostile-input fix that breaks real miners: the failure
    // mode of over-tightening is silently rejecting everyone's payout address.
    CHECK(decode("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4").has_value());
    CHECK(decode("tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx").has_value());
    CHECK(decode("bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0").has_value());
    CHECK(decode("1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa").has_value());
    CHECK(decode("3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy").has_value());
    CHECK(decode("bitcoincash:qpm2qsznhks23z7629mms6s4cwef74vcwvy22gdx6a").has_value());
}
