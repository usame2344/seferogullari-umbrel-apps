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
// File:        utils.hpp
// Description: Common helpers: hex/endian conversion, ntime validation and difficulty math. No network or DB.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/kdf.h>

namespace mkpool::utils {

inline std::atomic<std::uint64_t> g_job_counter{0};

// ---------- Hex ----------
inline std::uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return std::uint8_t(c - '0');
    if (c >= 'a' && c <= 'f') return std::uint8_t(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return std::uint8_t(c - 'A' + 10);
    throw std::runtime_error("invalid hex");
}

inline std::vector<std::uint8_t> hex_to_bytes(std::string_view h) {
    if (h.size() % 2) throw std::runtime_error("odd hex length");
    std::vector<std::uint8_t> out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i < h.size(); i += 2) {
        out.push_back(std::uint8_t((hex_nibble(h[i]) << 4) | hex_nibble(h[i+1])));
    }
    return out;
}

inline std::string bytes_to_hex(std::span<const std::uint8_t> data) {
    static constexpr char L[] = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (auto b : data) { s.push_back(L[b >> 4]); s.push_back(L[b & 0xf]); }
    return s;
}

inline std::string to_hex(std::string_view raw) {
    return bytes_to_hex({reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()});
}

inline bool valid_hex(std::string_view s) noexcept {
    for (auto c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return !s.empty();
}

// ---------- Integer hex encoding ----------
inline std::string int_to_hex(std::uint32_t v, int width = 8) {
    static constexpr char L[] = "0123456789abcdef";
    std::string s(static_cast<std::size_t>(width), '0');
    for (int i = width - 1; i >= 0; --i) {
        s[static_cast<std::size_t>(i)] = L[v & 0xf];
        v >>= 4;
    }
    return s;
}

inline std::string le_hex_u64(std::uint64_t v) {
    std::string s(16, '0');
    static constexpr char L[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        std::uint8_t b = std::uint8_t((v >> (8*i)) & 0xff);
        s[static_cast<std::size_t>(i*2  )] = L[b >> 4];
        s[static_cast<std::size_t>(i*2+1)] = L[b & 0xf];
    }
    return s;
}

inline std::string le_hex_u32(std::uint32_t v) {
    return le_hex_u64(v).substr(0, 8);
}

// ---------- Varint encoding ----------
inline std::string encode_varint(std::uint64_t v) {
    static constexpr char L[] = "0123456789abcdef";
    auto put_be = [&](std::string& s, std::uint64_t value, int bytes) {
        // bitcoin varint LE encoding
        for (int i = 0; i < bytes; ++i) {
            std::uint8_t b = std::uint8_t((value >> (8*i)) & 0xff);
            s.push_back(L[b >> 4]);
            s.push_back(L[b & 0xf]);
        }
    };
    std::string s;
    if (v < 0xfd) {
        s.push_back(L[(v >> 4) & 0xf]); s.push_back(L[v & 0xf]);
    } else if (v <= 0xffff) {
        s += "fd"; put_be(s, v, 2);
    } else if (v <= 0xffffffffull) {
        s += "fe"; put_be(s, v, 4);
    } else {
        s += "ff"; put_be(s, v, 8);
    }
    return s;
}

// ---------- ID generators ----------
inline std::string generate_extranonce1() {
    // 4 bytes / 8 hex chars: matches the de-facto Stratum standard used by
    // the major pools and every mining-rig proxy in the wild (PowerPlay-BMS,
    // NiceHash, MRR, etc.). Advertising 8 bytes here while proxies/ASICs
    // assume 4 caused merkle-mismatch on every submit.
    static thread_local std::mt19937_64 gen{ std::random_device{}() };
    std::uint32_t v = std::uint32_t(gen());
    return int_to_hex(v, 8); // 8 hex chars (4 bytes), MSB-first
}

inline std::string generate_session_id() {
    static thread_local std::mt19937_64 gen{ std::random_device{}() };
    std::uint32_t v = std::uint32_t(gen());
    return int_to_hex(v, 8);
}

inline std::string generate_job_id() {
    static thread_local std::mt19937_64 gen{ std::random_device{}() };
    std::uint64_t counter = g_job_counter.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t combined = (gen() & 0xffffffffull) ^ counter;
    return le_hex_u64(combined);
}

inline std::string generate_job_id_32() {
    static thread_local std::mt19937 gen{ std::random_device{}() };
    std::uint32_t counter = static_cast<std::uint32_t>(g_job_counter.fetch_add(1, std::memory_order_relaxed));
    std::uint32_t combined = gen() ^ counter;
    return int_to_hex(combined, 8); // 8 hex chars (4 bytes)
}


// ---------- SHA256 ----------
inline std::array<std::uint8_t, 32> sha256_once(std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 32> out{};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
    unsigned int len = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, out.data(), &len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("sha256 failed");
    }
    EVP_MD_CTX_free(ctx);
    return out;
}

inline std::array<std::uint8_t, 32> sha256d_arr(std::span<const std::uint8_t> data) {
    auto h1 = sha256_once(data);
    return sha256_once({h1.data(), h1.size()});
}

inline std::vector<std::uint8_t> sha256d_vec(std::span<const std::uint8_t> data) {
    auto h = sha256d_arr(data);
    return std::vector<std::uint8_t>(h.begin(), h.end());
}

inline std::array<std::uint8_t, 32> scrypt_1024_1_1_32(std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 32> out{};
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_SCRYPT, nullptr);
    if (!pctx) throw std::runtime_error("EVP_PKEY_CTX_new_id SCRYPT failed");
    if (EVP_PKEY_derive_init(pctx) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        throw std::runtime_error("EVP_PKEY_derive_init failed");
    }
    if (EVP_PKEY_CTX_set1_pbe_pass(pctx, reinterpret_cast<const char*>(data.data()), data.size()) <= 0 ||
        EVP_PKEY_CTX_set1_scrypt_salt(pctx, data.data(), data.size()) <= 0 ||
        EVP_PKEY_CTX_set_scrypt_N(pctx, 1024) <= 0 ||
        EVP_PKEY_CTX_set_scrypt_r(pctx, 1) <= 0 ||
        EVP_PKEY_CTX_set_scrypt_p(pctx, 1) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        throw std::runtime_error("EVP_PKEY_CTX_set_scrypt parameters failed");
    }
    size_t outlen = 32;
    if (EVP_PKEY_derive(pctx, out.data(), &outlen) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        throw std::runtime_error("EVP_PKEY_derive failed");
    }
    EVP_PKEY_CTX_free(pctx);
    return out;
}


// ---------- Coinbase / block helpers ----------
inline std::string rebuild_coinbase(std::string_view cb1, std::string_view cb2,
                                    std::string_view full_extranonce) {
    std::string out;
    out.reserve(cb1.size() + cb2.size() + full_extranonce.size());
    out.append(cb1); out.append(full_extranonce); out.append(cb2);
    return out;
}

// Reverse a hex string byte-by-byte (each 2 chars = 1 byte).
// "0123abcd" -> "cdab2301"
inline std::string byte_reverse_hex(std::string_view hex) {
    std::string out;
    out.resize(hex.size());
    const std::size_t n = hex.size() / 2;
    for (std::size_t i = 0; i < n; ++i) {
        out[i * 2]     = hex[(n - 1 - i) * 2];
        out[i * 2 + 1] = hex[(n - 1 - i) * 2 + 1];
    }
    return out;
}

inline std::string build_block_header(std::string_view version_hex_le,
                                      std::string_view prevhash_hex,
                                      std::string_view merkleroot_hex,
                                      std::string_view ntime_hex,
                                      std::string_view bits_hex,
                                      std::string_view nonce_hex) {
    std::string out;
    out.reserve(160);
    out.append(version_hex_le).append(prevhash_hex).append(merkleroot_hex)
       .append(ntime_hex).append(bits_hex).append(nonce_hex);
    return out;
}

// Compute coinbase txid in INTERNAL form (raw SHA-256d output bytes), which is
// the form merkle math operates on natively. Display form is byte-reverse of
// this. The function name is historical; output is the raw SHA bytes hex.
inline std::string compute_coinbase_txid_le(std::string_view coinbase_hex) {
    auto bytes = hex_to_bytes(coinbase_hex);
    auto h = sha256d_arr(bytes);
    return bytes_to_hex(h);
}

inline std::string assemble_full_block(std::string_view header_hex,
                                       std::string_view coinbase_hex,
                                       std::span<const std::string> txs_hex) {
    std::string out;
    out.reserve(header_hex.size() + coinbase_hex.size() + 200 + txs_hex.size() * 600);
    out.append(header_hex);
    out.append(encode_varint(1 + txs_hex.size()));
    out.append(coinbase_hex);
    for (const auto& tx : txs_hex) out.append(tx);
    return out;
}

// ---------- Compact target ----------
inline std::array<std::uint8_t, 32> target_from_bits_hex(std::string_view bits_hex) {
    std::array<std::uint8_t, 32> out{};
    if (bits_hex.size() != 8) return out;
    auto b = hex_to_bytes(bits_hex);
    std::uint8_t exp = b[0];
    std::uint32_t mant = (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | b[3];
    if (mant == 0) return out;
    int shift = int(exp) - 3;
    if (shift < 0) {
        std::uint32_t m = mant >> (std::uint32_t(-shift) * 8);
        out[29] = std::uint8_t((m >> 16) & 0xff);
        out[30] = std::uint8_t((m >>  8) & 0xff);
        out[31] = std::uint8_t( m        & 0xff);
    } else {
        int start = 32 - (3 + shift);
        if (start < 0) return out;
        out[std::size_t(start    )] = std::uint8_t((mant >> 16) & 0xff);
        out[std::size_t(start + 1)] = std::uint8_t((mant >>  8) & 0xff);
        out[std::size_t(start + 2)] = std::uint8_t( mant        & 0xff);
    }
    return out;
}

// ---------- eCash Real-Time Target (RTT) ----------
// Mirrors bitcoin-abc GetNextRTTWorkRequired (src/policy/block/rtt.cpp). eCash
// avalanche enforces RTT as a *parking policy*: a block arriving too soon after
// its parent must beat a target harder than the standard DAA target, or the
// network parks it (policy-bad-rtt) and it is orphaned. RTT is XEC-only; every
// caller here is gated on ChainKind::eCash, so this code is never reached for
// any other chain.

// arith_uint256::SetDouble followed by GetCompact(): truncate to an integer,
// keep the top 3 significant bytes as the mantissa and the byte-count as the
// exponent. Validated to reproduce the node's rtt.nexttarget exactly.
inline std::uint32_t target_double_to_compact(long double t) {
    if (!(t >= 1.0L)) return 0;                  // <1 (or NaN) => hardest possible
    int exp2 = 0;
    (void)std::frexp(static_cast<double>(t), &exp2); // t in [2^(exp2-1), 2^exp2)
    // frexp on the double cast is precise enough for the exponent (bit length).
    int nbits = exp2;                            // bit length of floor(t)
    int size  = (nbits + 7) / 8;
    long double shifted = (size <= 3)
        ? t * std::pow(2.0L, 8 * (3 - size))
        : t / std::pow(2.0L, 8 * (size - 3));
    std::uint32_t compact = static_cast<std::uint32_t>(std::floor(shifted));
    if (compact & 0x00800000u) { compact >>= 8; ++size; }
    compact |= static_cast<std::uint32_t>(size) << 24;
    return compact;
}

// Returns true if a block whose PoW hash is `hash_be` (32 bytes, big-endian,
// [0]=MSB) satisfies the eCash real-time target evaluated at `now` (unix
// seconds; use current wall-clock, which the network's later receive-time check
// meets or beats -> conservative, never a false accept). `prevheadertime` are
// the node receive times of the previous blocks at depths {1,2,5,11,17}
// (GBT rtt.prevheadertime), `prevbits` the previous block's compact nBits
// (GBT rtt.prevbits). Fails safe to TRUE whenever RTT data is unavailable.
inline bool ecash_rtt_ok(const std::array<std::uint32_t, 5>& prevheadertime,
                         std::uint32_t prevbits, std::int64_t now,
                         const std::array<std::uint8_t, 32>& hash_be) {
    constexpr double RTT_K = 6.0;
    // FACT[k] = RTT_K * tgamma(1 + 1/RTT_K)^RTT_K / T[k]^(RTT_K-1) for the
    // per-window target block times below; computed once. Matches ABC's
    // precomputed constants (reproduces rtt.nexttarget to the compact bit).
    static const std::array<double, 5> FACT = [] {
        const double T[5] = {150.0, 600.0, 2400.0, 6000.0, 9600.0};
        const double g = std::pow(std::tgamma(1.0 + 1.0 / RTT_K), RTT_K);
        std::array<double, 5> f{};
        for (int i = 0; i < 5; ++i) f[i] = RTT_K * g / std::pow(T[i], RTT_K - 1.0);
        return f;
    }();

    const std::uint32_t exp  = prevbits >> 24;
    const std::uint32_t mant = prevbits & 0x007fffffu;
    if (mant == 0 || exp < 3 || exp > 32) return true;        // degenerate => don't gate
    const long double prevTarget =
        static_cast<long double>(mant) * std::pow(2.0L, static_cast<long double>(8 * (int(exp) - 3)));

    long double minTarget = 0.0L;
    bool have = false;
    for (int k = 0; k < 5; ++k) {
        if (prevheadertime[k] == 0) return true;              // node lacked 17-block history
        long double dt = static_cast<long double>(now - static_cast<std::int64_t>(prevheadertime[k]));
        if (dt < 1.0L) dt = 1.0L;
        const long double tgt =
            prevTarget * static_cast<long double>(FACT[k]) * std::pow(dt, static_cast<long double>(RTT_K - 1.0));
        if (!have || tgt < minTarget) { minTarget = tgt; have = true; }
    }
    if (!have) return true;
    // Target easier than any 256-bit value => RTT not binding; the block already
    // met the DAA target upstream, so it meets RTT too.
    if (minTarget >= std::pow(2.0L, 256.0L)) return true;

    const std::uint32_t compact = target_double_to_compact(minTarget);
    if ((compact >> 24) > 32u) return true;                   // beyond 256 bits => not binding
    const auto tgt_be = target_from_bits_hex(int_to_hex(compact, 8));
    for (std::size_t i = 0; i < 32; ++i) {
        if (hash_be[i] != tgt_be[i]) return hash_be[i] < tgt_be[i];
    }
    return true;                                              // exactly equal meets the target
}

inline double bits_to_difficulty(std::string_view bits_hex) {
    // bits hex is presented to miners as the 4 header bytes in their
    // header-display order (the same big-endian-style hex the template gives).
    // bits = exponent | mantissa
    auto bytes = hex_to_bytes(bits_hex);
    if (bytes.size() != 4) return 0.0;
    std::uint8_t  exp = bytes[0];
    std::uint32_t mant = (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
    if (mant == 0) return 0.0;
    double diff = double(0x0000ffff) / double(mant);
    int n = exp;
    while (n < 29) { diff *= 256.0; ++n; }
    while (n > 29) { diff /= 256.0; --n; }
    return diff;
}

// ---------- BIP34 + script push ----------
inline std::string encode_bip34_height(std::int32_t height) {
    if (height < 0) throw std::runtime_error("negative height");
    std::vector<std::uint8_t> b;
    std::uint32_t h = std::uint32_t(height);
    while (h) { b.push_back(std::uint8_t(h & 0xff)); h >>= 8; }
    if (b.empty()) b.push_back(0);
    std::string s;
    s.reserve(2 + b.size() * 2);
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", int(b.size()));
    s.append(buf, 2);
    s += bytes_to_hex(b);
    return s;
}

inline std::string encode_push_bytes(std::span<const std::uint8_t> data) {
    if (data.size() > 0x4b) throw std::runtime_error("push too long");
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", int(data.size()));
    return std::string(buf, 2) + bytes_to_hex(data);
}

inline std::string encode_push_ascii(std::string_view s) {
    return encode_push_bytes({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
}

// ---------- Time validation ----------
inline bool valid_ntime(std::uint32_t submitted, std::uint32_t mintime,
                        std::uint32_t now_unix, std::uint32_t future_slack = 7200) {
    if (submitted < mintime) return false;
    if (submitted > now_unix + future_slack) return false;
    return true;
}

} // namespace mkpool::utils
