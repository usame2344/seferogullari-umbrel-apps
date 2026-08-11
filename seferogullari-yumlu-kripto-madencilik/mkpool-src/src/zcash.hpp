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
// File:        zcash.hpp
// Description: Zcash per-miner coinbase: redirects the miner-reward output while preserving funding-stream outputs and recomputes the v5 txid per ZIP-244.
// Created:     2026-05-31
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <sodium.h>

#include "utils.hpp"

namespace mkpool::zcash {

using Bytes = std::vector<std::uint8_t>;

// BLAKE2b-256 with a 16-byte personalization (no key, zero salt).
inline std::array<std::uint8_t, 32>
blake2b_256_personal(const char person[16], const std::uint8_t* data, std::size_t len) {
    std::array<std::uint8_t, 32> out{};
    unsigned char salt[16] = {0};
    crypto_generichash_blake2b_salt_personal(
        out.data(), out.size(), data, len, nullptr, 0,
        salt, reinterpret_cast<const unsigned char*>(person));
    return out;
}

inline std::array<std::uint8_t, 32>
blake2b_256_personal(const char person[16], const Bytes& data) {
    return blake2b_256_personal(person, data.data(), data.size());
}

// Read a Bitcoin/Zcash CompactSize from buf at pos; advances pos.
inline std::uint64_t read_compact(const Bytes& b, std::size_t& pos) {
    if (pos >= b.size()) throw std::runtime_error("zcash: compact OOB");
    std::uint8_t n = b[pos++];
    auto rd = [&](int k) {
        std::uint64_t v = 0;
        for (int i = 0; i < k; ++i) v |= std::uint64_t(b.at(pos++)) << (8 * i);
        return v;
    };
    if (n < 0xfd) return n;
    if (n == 0xfd) return rd(2);
    if (n == 0xfe) return rd(4);
    return rd(8);
}

inline Bytes write_compact(std::uint64_t n) {
    Bytes o;
    if (n < 0xfd) { o.push_back(std::uint8_t(n)); }
    else if (n <= 0xffff) { o.push_back(0xfd); o.push_back(n & 0xff); o.push_back((n >> 8) & 0xff); }
    else { o.push_back(0xfe); for (int i = 0; i < 4; ++i) o.push_back((n >> (8 * i)) & 0xff); }
    return o;
}

// Parsed transparent layout of a v5 coinbase (offsets into the raw tx bytes).
struct CoinbaseLayout {
    std::size_t vout_count_pos{0}; // first byte of the vout-count compactsize
    std::size_t outputs_start{0};  // first byte of first output (after vout count)
    std::size_t outputs_end{0};    // one past last output
    // [value(8), script_start, script_len] for each output
    std::vector<std::array<std::size_t, 3>> outputs;
};

inline CoinbaseLayout parse_layout(const Bytes& raw) {
    CoinbaseLayout L;
    std::size_t p = 20;                         // header: ver,vgid,branch,lock,expiry
    std::uint64_t vin = read_compact(raw, p);
    for (std::uint64_t i = 0; i < vin; ++i) {
        p += 36;                                // outpoint
        std::uint64_t sl = read_compact(raw, p);
        p += sl;                                // scriptSig
        p += 4;                                 // sequence
    }
    L.vout_count_pos = p;
    std::uint64_t vout = read_compact(raw, p);
    L.outputs_start = p;
    for (std::uint64_t i = 0; i < vout; ++i) {
        std::size_t vstart = p;
        p += 8;
        std::uint64_t sl = read_compact(raw, p);
        L.outputs.push_back({vstart, p, static_cast<std::size_t>(sl)});
        p += sl;
    }
    L.outputs_end = p;
    return L;
}

// ZIP-244 txid of a v5 transaction whose Sapling and Orchard bundles are empty
// (always true for a coinbase). Returns the 32-byte txid in internal order.
inline std::array<std::uint8_t, 32> zip244_txid(const Bytes& raw) {
    Bytes header(raw.begin(), raw.begin() + 20);
    std::array<std::uint8_t, 4> branch{raw[8], raw[9], raw[10], raw[11]};

    std::size_t p = 20;
    std::uint64_t vin = read_compact(raw, p);
    Bytes prevouts, sequences;
    for (std::uint64_t i = 0; i < vin; ++i) {
        prevouts.insert(prevouts.end(), raw.begin() + p, raw.begin() + p + 36);
        p += 36;
        std::uint64_t sl = read_compact(raw, p);
        p += sl;
        sequences.insert(sequences.end(), raw.begin() + p, raw.begin() + p + 4);
        p += 4;
    }
    std::uint64_t vout = read_compact(raw, p);
    std::size_t ods = p;
    for (std::uint64_t i = 0; i < vout; ++i) {
        p += 8;
        std::uint64_t sl = read_compact(raw, p);
        p += sl;
    }
    Bytes outputs(raw.begin() + ods, raw.begin() + p);

    auto hd  = blake2b_256_personal("ZTxIdHeadersHash", header);
    auto pd  = blake2b_256_personal("ZTxIdPrevoutHash", prevouts);
    auto sqd = blake2b_256_personal("ZTxIdSequencHash", sequences);
    auto od  = blake2b_256_personal("ZTxIdOutputsHash", outputs);
    Bytes tcat;
    tcat.insert(tcat.end(), pd.begin(), pd.end());
    tcat.insert(tcat.end(), sqd.begin(), sqd.end());
    tcat.insert(tcat.end(), od.begin(), od.end());
    auto td = blake2b_256_personal("ZTxIdTranspaHash", tcat);
    auto sd = blake2b_256_personal("ZTxIdSaplingHash", nullptr, 0);
    auto orchd = blake2b_256_personal("ZTxIdOrchardHash", nullptr, 0);

    Bytes msg;
    msg.insert(msg.end(), hd.begin(), hd.end());
    msg.insert(msg.end(), td.begin(), td.end());
    msg.insert(msg.end(), sd.begin(), sd.end());
    msg.insert(msg.end(), orchd.begin(), orchd.end());

    char person[16];
    std::memcpy(person, "ZcashTxHash_", 12);
    std::memcpy(person + 12, branch.data(), 4);
    return blake2b_256_personal(person, msg);
}

// ---- base58check decode (for Zcash transparent addresses) ----
inline bool b58_decode(std::string_view s, Bytes& out) {
    static const char* A = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    std::vector<std::uint8_t> b;
    for (char c : s) {
        const char* pos = std::strchr(A, c);
        if (!pos || c == 0) return false;
        int carry = int(pos - A);
        for (auto& d : b) { carry += 58 * d; d = carry & 0xff; carry >>= 8; }
        while (carry) { b.push_back(carry & 0xff); carry >>= 8; }
    }
    for (char c : s) { if (c == '1') b.push_back(0); else break; }
    std::reverse(b.begin(), b.end());
    out = std::move(b);
    return true;
}

// Decode a Zcash transparent address (t1/t3 mainnet, tm/t2 testnet) into its
// scriptPubKey. Returns empty on failure.
inline Bytes taddr_to_script(std::string_view addr) {
    Bytes raw;
    if (!b58_decode(addr, raw) || raw.size() != 26) return {};   // 2 ver + 20 h160 + 4 chk
    // verify checksum
    Bytes body(raw.begin(), raw.end() - 4);
    auto chk = utils::sha256d_arr({body.data(), body.size()});
    if (!std::equal(raw.end() - 4, raw.end(), chk.begin())) return {};
    std::uint16_t ver = (std::uint16_t(raw[0]) << 8) | raw[1];
    Bytes h160(raw.begin() + 2, raw.begin() + 22);
    Bytes script;
    // P2PKH: t1 (0x1cb8), tm (0x1d25)
    // P2SH : t3 (0x1cbd), t2 (0x1cba)
    if (ver == 0x1cb8 || ver == 0x1d25) {
        script = {0x76, 0xa9, 0x14};
        script.insert(script.end(), h160.begin(), h160.end());
        script.push_back(0x88); script.push_back(0xac);
    } else if (ver == 0x1cbd || ver == 0x1cba) {
        script = {0xa9, 0x14};
        script.insert(script.end(), h160.begin(), h160.end());
        script.push_back(0x87);
    } else {
        return {};
    }
    return script;
}

// Redirect the largest-value (miner-reward) output to miner_script, preserving
// all other (funding-stream) outputs. If donation_script/donation_percent are
// given, the miner reward is split into a miner output and an operator-donation
// output. Returns the rebuilt raw coinbase (with corrected vout count), or empty
// on failure.
inline Bytes append_output(Bytes& outs, std::uint64_t value, const Bytes& script) {
    for (int b = 0; b < 8; ++b) outs.push_back(std::uint8_t((value >> (8 * b)) & 0xff));
    Bytes c = write_compact(script.size());
    outs.insert(outs.end(), c.begin(), c.end());
    outs.insert(outs.end(), script.begin(), script.end());
    return outs;
}

inline Bytes swap_miner_output(const Bytes& raw, const Bytes& miner_script,
                               const Bytes& donation_script = {},
                               double donation_percent = 0.0) {
    CoinbaseLayout L = parse_layout(raw);
    if (L.outputs.empty() || miner_script.empty()) return {};
    std::size_t mi = 0; std::uint64_t best = 0;
    for (std::size_t i = 0; i < L.outputs.size(); ++i) {
        std::uint64_t v = 0;
        for (int b = 0; b < 8; ++b) v |= std::uint64_t(raw[L.outputs[i][0] + b]) << (8 * b);
        if (v >= best) { best = v; mi = i; }
    }
    std::uint64_t miner_val = best;
    std::uint64_t donation_val = 0;
    bool split = !donation_script.empty() && donation_percent > 0.0;
    if (split) {
        donation_val = static_cast<std::uint64_t>(
            (long double)miner_val * (long double)donation_percent / 100.0L + 0.5L);
        if (donation_val == 0 || donation_val >= miner_val) { split = false; donation_val = 0; }
    }

    Bytes newouts;
    std::size_t out_count = 0;
    for (std::size_t i = 0; i < L.outputs.size(); ++i) {
        if (i == mi) {
            append_output(newouts, miner_val - donation_val, miner_script); ++out_count;
            if (split) { append_output(newouts, donation_val, donation_script); ++out_count; }
        } else {
            std::size_t vstart = L.outputs[i][0], sstart = L.outputs[i][1], slen = L.outputs[i][2];
            newouts.insert(newouts.end(), raw.begin() + vstart, raw.begin() + vstart + 8);
            Bytes c = write_compact(slen);
            newouts.insert(newouts.end(), c.begin(), c.end());
            newouts.insert(newouts.end(), raw.begin() + sstart, raw.begin() + sstart + slen);
            ++out_count;
        }
    }
    Bytes out;
    out.insert(out.end(), raw.begin(), raw.begin() + L.vout_count_pos);
    Bytes cnt = write_compact(out_count);
    out.insert(out.end(), cnt.begin(), cnt.end());
    out.insert(out.end(), newouts.begin(), newouts.end());
    out.insert(out.end(), raw.begin() + L.outputs_end, raw.end());
    return out;
}

} // namespace mkpool::zcash
