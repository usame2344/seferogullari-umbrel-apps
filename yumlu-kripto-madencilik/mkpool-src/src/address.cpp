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
// File:        address.cpp
// Description: Address decoders (base58check, bech32/bech32m, CashAddr) and scriptPubKey builders. No network calls.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "address.hpp"

#include <openssl/sha.h>
#include <openssl/evp.h>

namespace mkpool::address {

namespace {

// ---------- Common helpers ----------

inline std::uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
    return 0xff;
}

std::string to_hex(std::span<const std::uint8_t> data) {
    static constexpr char L[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (auto b : data) {
        out.push_back(L[b >> 4]);
        out.push_back(L[b & 0x0f]);
    }
    return out;
}

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data) {
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

// ---------- Base58Check ----------

constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

std::expected<std::vector<std::uint8_t>, Error>
base58_decode(std::string_view s) noexcept {
    std::array<int, 128> map{};
    map.fill(-1);
    for (int i = 0; i < (int)kBase58Alphabet.size(); ++i)
        map[(unsigned char)kBase58Alphabet[i]] = i;

    // Count leading '1's = leading zero bytes
    std::size_t zeros = 0;
    while (zeros < s.size() && s[zeros] == '1') ++zeros;

    // Big-num accumulator using base-256 representation.
    std::vector<std::uint8_t> b256(s.size() * 733 / 1000 + 1, 0);
    for (std::size_t i = zeros; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 128 || map[c] < 0) return std::unexpected(Error::BadCharacter);
        int carry = map[c];
        for (auto it = b256.rbegin(); it != b256.rend(); ++it) {
            carry += 58 * (*it);
            *it = (std::uint8_t)(carry % 256);
            carry /= 256;
        }
        if (carry) return std::unexpected(Error::BadLength);
    }

    // Skip leading zero bytes in b256.
    auto first_non_zero = std::ranges::find_if(b256, [](auto b){ return b != 0; });
    std::vector<std::uint8_t> out(zeros, 0);
    out.insert(out.end(), first_non_zero, b256.end());
    return out;
}

bool base58check_verify(std::span<const std::uint8_t> payload) {
    if (payload.size() < 5) return false;
    auto body = payload.subspan(0, payload.size() - 4);
    auto sum  = payload.subspan(payload.size() - 4);
    auto h1 = sha256(body);
    auto h2 = sha256(h1);
    return std::memcmp(h2.data(), sum.data(), 4) == 0;
}

// ---------- Bech32 / Bech32m (BIP173/BIP350) ----------

constexpr std::string_view kBech32Charset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

constexpr std::uint32_t kBech32Const  = 1;
constexpr std::uint32_t kBech32mConst = 0x2bc830a3;

std::uint32_t bech32_polymod(std::span<const std::uint8_t> values) {
    constexpr std::uint32_t G[5] = {
        0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3 };
    std::uint32_t chk = 1;
    for (auto v : values) {
        std::uint32_t b = chk >> 25;
        chk = ((chk & 0x1ffffff) << 5) ^ v;
        for (int i = 0; i < 5; ++i)
            if ((b >> i) & 1) chk ^= G[i];
    }
    return chk;
}

std::vector<std::uint8_t> bech32_hrp_expand(std::string_view hrp) {
    std::vector<std::uint8_t> out;
    out.reserve(hrp.size() * 2 + 1);
    for (auto c : hrp) out.push_back((std::uint8_t)(c >> 5));
    out.push_back(0);
    for (auto c : hrp) out.push_back((std::uint8_t)(c & 31));
    return out;
}

std::expected<std::pair<int, std::vector<std::uint8_t>>, Error>
bech32_decode(std::string_view hrp_expected, std::string_view s) noexcept {
    // Mixed-case is forbidden by BIP173.
    bool has_upper = false, has_lower = false;
    for (auto c : s) {
        if (c >= 'A' && c <= 'Z') has_upper = true;
        else if (c >= 'a' && c <= 'z') has_lower = true;
    }
    if (has_upper && has_lower) return std::unexpected(Error::BadCharacter);

    std::string lower;
    lower.reserve(s.size());
    for (auto c : s) lower.push_back((char)std::tolower((unsigned char)c));

    auto pos = lower.rfind('1');
    if (pos == std::string::npos || pos < 1 || pos + 7 > lower.size())
        return std::unexpected(Error::BadLength);

    std::string_view hrp(lower.data(), pos);
    if (hrp != hrp_expected) return std::unexpected(Error::UnknownPrefix);

    std::vector<std::uint8_t> values;
    values.reserve(lower.size() - pos - 1);
    for (std::size_t i = pos + 1; i < lower.size(); ++i) {
        auto idx = kBech32Charset.find(lower[i]);
        if (idx == std::string_view::npos) return std::unexpected(Error::BadCharacter);
        values.push_back((std::uint8_t)idx);
    }

    auto expand = bech32_hrp_expand(hrp);
    expand.insert(expand.end(), values.begin(), values.end());
    auto chk = bech32_polymod(expand);
    int encoding;
    if      (chk == kBech32Const)  encoding = 0; // bech32
    else if (chk == kBech32mConst) encoding = 1; // bech32m
    else return std::unexpected(Error::BadChecksum);

    // strip 6-byte checksum
    values.resize(values.size() - 6);
    return std::pair{encoding, std::move(values)};
}

std::expected<std::vector<std::uint8_t>, Error>
convert_bits(std::span<const std::uint8_t> in, int from_bits, int to_bits, bool pad) {
    std::uint32_t acc = 0;
    int bits = 0;
    std::vector<std::uint8_t> out;
    std::uint32_t maxv = (1u << to_bits) - 1;
    std::uint32_t maxa = (1u << (from_bits + to_bits - 1)) - 1;
    for (auto v : in) {
        if ((v >> from_bits) != 0) return std::unexpected(Error::BadCharacter);
        acc = ((acc << from_bits) | v) & maxa;
        bits += from_bits;
        while (bits >= to_bits) {
            bits -= to_bits;
            out.push_back((std::uint8_t)((acc >> bits) & maxv));
        }
    }
    if (pad) {
        if (bits) out.push_back((std::uint8_t)((acc << (to_bits - bits)) & maxv));
    } else if (bits >= from_bits || (acc << (to_bits - bits)) & maxv) {
        return std::unexpected(Error::BadLength);
    }
    return out;
}

struct SegwitInfo {
    int version;
    std::vector<std::uint8_t> program;
    int encoding; // 0=bech32, 1=bech32m
};

std::expected<SegwitInfo, Error>
segwit_decode(std::string_view hrp, std::string_view s) noexcept {
    auto dec = bech32_decode(hrp, s);
    if (!dec) return std::unexpected(dec.error());
    auto& [enc, data] = *dec;
    if (data.empty()) return std::unexpected(Error::BadLength);
    int version = data[0];
    if (version > 16) return std::unexpected(Error::UnsupportedVersion);
    auto prog = convert_bits({data.data()+1, data.size()-1}, 5, 8, false);
    if (!prog) return std::unexpected(prog.error());
    if (prog->size() < 2 || prog->size() > 40) return std::unexpected(Error::BadLength);
    if (version == 0 && prog->size() != 20 && prog->size() != 32)
        return std::unexpected(Error::BadLength);
    // BIP350: v0 -> bech32, v>=1 -> bech32m
    if ((version == 0 && enc != 0) || (version >= 1 && enc != 1))
        return std::unexpected(Error::BadChecksum);
    return SegwitInfo{version, std::move(*prog), enc};
}

// ---------- CashAddr (BCH) ----------

constexpr std::string_view kCashCharset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

std::uint64_t cashaddr_polymod(std::span<const std::uint8_t> v) {
    constexpr std::uint64_t G[5] = {
        0x98f2bc8e61ull, 0x79b76d99e2ull, 0xf33e5fb3c4ull,
        0xae2eabe2a8ull, 0x1e4f43e470ull
    };
    std::uint64_t c = 1;
    for (auto d : v) {
        std::uint8_t c0 = (std::uint8_t)(c >> 35);
        c = ((c & 0x07ffffffffull) << 5) ^ d;
        for (int i = 0; i < 5; ++i)
            if ((c0 >> i) & 1) c ^= G[i];
    }
    return c ^ 1;
}

std::vector<std::uint8_t> cashaddr_expand_prefix(std::string_view prefix) {
    std::vector<std::uint8_t> out;
    out.reserve(prefix.size() + 1);
    for (auto c : prefix) out.push_back((std::uint8_t)(c & 0x1f));
    out.push_back(0);
    return out;
}

struct CashDecoded {
    std::string prefix;
    std::uint8_t type;  // 0=P2KH, 1=P2SH
    std::vector<std::uint8_t> hash; // 20 bytes typically
};

std::expected<CashDecoded, Error> cashaddr_decode(std::string_view s) noexcept {
    // Either "prefix:payload" or just "payload" with default prefix.
    std::string prefix;
    std::string_view payload;
    auto colon = s.find(':');
    if (colon != std::string_view::npos) {
        prefix.assign(s.substr(0, colon));
        payload = s.substr(colon + 1);
    } else {
        prefix = "bitcoincash";
        payload = s;
    }
    // Case: enforce all lower or all upper.
    bool has_upper = false, has_lower = false;
    for (auto c : payload) {
        if (c >= 'A' && c <= 'Z') has_upper = true;
        else if (c >= 'a' && c <= 'z') has_lower = true;
    }
    if (has_upper && has_lower) return std::unexpected(Error::BadCharacter);
    for (auto& c : prefix) c = (char)std::tolower((unsigned char)c);
    std::string lower(payload);
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);

    std::vector<std::uint8_t> values;
    values.reserve(lower.size());
    for (auto c : lower) {
        auto idx = kCashCharset.find(c);
        if (idx == std::string_view::npos) return std::unexpected(Error::BadCharacter);
        values.push_back((std::uint8_t)idx);
    }
    if (values.size() < 8) return std::unexpected(Error::BadLength);

    auto expand = cashaddr_expand_prefix(prefix);
    expand.insert(expand.end(), values.begin(), values.end());
    if (cashaddr_polymod(expand) != 0) return std::unexpected(Error::BadChecksum);

    values.resize(values.size() - 8); // strip 40-bit checksum

    auto bytes = convert_bits(values, 5, 8, false);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->empty()) return std::unexpected(Error::BadLength);

    std::uint8_t version = (*bytes)[0];
    std::uint8_t type = (version >> 3) & 0x1f;
    std::uint8_t size_bits = version & 0x07;
    constexpr std::array<int, 8> kSizeMap{160,192,224,256,320,384,448,512};
    int expected = kSizeMap[size_bits] / 8;
    if ((int)(bytes->size() - 1) != expected) return std::unexpected(Error::BadLength);

    CashDecoded out;
    out.prefix = std::move(prefix);
    out.type = type;
    out.hash.assign(bytes->begin()+1, bytes->end());
    return out;
}

} // anonymous

// ============================================================================

// Internal decoder. MAY THROW -- see the decode() wrapper below, which is the
// noexcept boundary. Keep the risky work in here, not in the wrapper.
static std::expected<Decoded, Error> decode_impl(std::string_view addr) {
    if (addr.empty()) return std::unexpected(Error::Empty);

    // base58_decode is O(n^2) and nothing here bounds its input, so bound it at
    // the door: 128 is generous (BIP173 caps bech32 at 90; a prefixed cashaddr is
    // ~56; base58 P2PKH is ~35), and no valid address can exceed it. Callers do
    // check today (V1 authorize at 128, SV2 user_identity at 255 by wire format),
    // which is why this is not currently reachable with a long string -- but that
    // is a property of ten call sites rather than of this function.
    if (addr.size() > 128) return std::unexpected(Error::BadLength);

    // CashAddr always contains ':' OR begins with 'q'/'p' (no leading '1','3','b','t').
    // Easiest heuristic: try cashaddr if prefix is set or starts with q/p/r/... but
    // also if explicit cashaddr prefix detected.
    auto colon = addr.find(':');
    bool is_prefixless_cash = false;
    if (colon == std::string_view::npos && !addr.empty()) {
        char first = (char)std::tolower((unsigned char)addr[0]);
        if (first == 'q' || first == 'p' || first == 'r') {
            is_prefixless_cash = true;
        }
    }
    if (colon != std::string_view::npos || is_prefixless_cash) {
        // Force-try cashaddr.
        auto cad = cashaddr_decode(addr);
        if (cad) {
            Decoded d;
            d.program = std::move(cad->hash);
            if (cad->prefix == "bitcoincash" || cad->prefix == "ecash" || cad->prefix == "bitcoincashii") d.network = Network::BchMainnet;
            else if (cad->prefix == "bchtest" || cad->prefix == "ectest") d.network = Network::BchTestnet;
            else if (cad->prefix == "bchreg" || cad->prefix == "ecreg")  d.network = Network::BchRegtest;
            else return std::unexpected(Error::UnsupportedNetwork);
            d.type = (cad->type == 0) ? Type::BchP2PKH : Type::BchP2SH;
            return d;
        }
        if (colon != std::string_view::npos) {
            return std::unexpected(cad.error());
        }
    }

    // Try bech32/bech32m first if matches known HRP prefixes.
    auto try_bech = [&](std::string_view hrp, Network net) -> std::optional<Decoded> {
        if (addr.size() > hrp.size()+1 &&
            std::tolower(addr[0]) == hrp[0] &&
            std::tolower(addr[1]) == (hrp.size() > 1 ? hrp[1] : '\0') &&
            (addr[hrp.size()] == '1' ||
             (hrp.size() >= 3 && addr.size() > hrp.size() &&
              std::tolower(addr[2]) == hrp[2]))) {
            auto sw = segwit_decode(hrp, addr);
            if (!sw) return std::nullopt;
            Decoded d;
            d.network = net;
            d.program = std::move(sw->program);
            if (sw->version == 0 && d.program.size() == 20) d.type = Type::P2WPKH;
            else if (sw->version == 0 && d.program.size() == 32) d.type = Type::P2WSH;
            else if (sw->version == 1 && d.program.size() == 32) d.type = Type::P2TR;
            else return std::nullopt;
            return d;
        }
        return std::nullopt;
    };

    if (auto d = try_bech("bc",   Network::BitcoinMainnet)) return *d;
    if (auto d = try_bech("tb",   Network::BitcoinTestnet)) return *d;
    if (auto d = try_bech("bcrt", Network::BitcoinRegtest)) return *d;
    if (auto d = try_bech("ltc",  Network::BitcoinMainnet)) return *d;
    if (auto d = try_bech("tltc", Network::BitcoinTestnet)) return *d;
    if (auto d = try_bech("dgb",  Network::BitcoinMainnet)) return *d;
    if (auto d = try_bech("dgbt", Network::BitcoinTestnet)) return *d;
    if (auto d = try_bech("tdgb", Network::BitcoinTestnet)) return *d;

    // Base58Check fallback (Legacy P2PKH / P2SH).
    auto raw = base58_decode(addr);
    if (raw) {
        if (raw->size() < 5) return std::unexpected(Error::BadLength);
        if (!base58check_verify(*raw)) return std::unexpected(Error::BadChecksum);
        if (raw->size() == 26) {
            // Zcash 2-byte version prefix
            std::uint16_t version = ((std::uint16_t)(*raw)[0] << 8) | (*raw)[1];
            std::vector<std::uint8_t> body(raw->begin()+2, raw->end()-4);
            if (body.size() != 20) return std::unexpected(Error::BadLength);
            Decoded d;
            d.program = std::move(body);
            if (version == 0x1cb8) {
                d.type = Type::P2PKH;
                d.network = Network::BitcoinMainnet;
            } else if (version == 0x1cbd) {
                d.type = Type::P2SH;
                d.network = Network::BitcoinMainnet;
            } else if (version == 0x1d25) {
                d.type = Type::P2PKH;
                d.network = Network::BitcoinTestnet;
            } else if (version == 0x1cba) {
                d.type = Type::P2SH;
                d.network = Network::BitcoinTestnet;
            } else {
                return std::unexpected(Error::UnsupportedVersion);
            }
            return d;
        } else {
            // Standard 1-byte version prefix
            std::uint8_t version = (*raw)[0];
            std::vector<std::uint8_t> body(raw->begin()+1, raw->end()-4);
            if (body.size() != 20) return std::unexpected(Error::BadLength);
            Decoded d;
            d.program = std::move(body);
            switch (version) {
                case 0x00: d.type = Type::P2PKH; d.network = Network::BitcoinMainnet; break;
                case 0x05: d.type = Type::P2SH;  d.network = Network::BitcoinMainnet; break; // BTC Mainnet P2SH, LTC Mainnet P2SH
                case 0x6f: d.type = Type::P2PKH; d.network = Network::BitcoinTestnet; break; // BTC/LTC Testnet P2PKH
                case 0xc4: d.type = Type::P2SH;  d.network = Network::BitcoinTestnet; break; // BTC/LTC/DOGE Testnet P2SH
                
                // Litecoin specific
                case 0x30: d.type = Type::P2PKH; d.network = Network::BitcoinMainnet; break; // LTC Mainnet P2PKH, DGB Mainnet P2PKH
                case 0x32: d.type = Type::P2SH;  d.network = Network::BitcoinMainnet; break; // LTC Mainnet P2SH
                case 0x3a: d.type = Type::P2SH;  d.network = Network::BitcoinTestnet; break; // LTC Testnet P2SH (alternate)
                
                // Dogecoin specific
                case 0x1e: d.type = Type::P2PKH; d.network = Network::BitcoinMainnet; break; // DOGE Mainnet P2PKH, DGB Mainnet P2PKH
                case 0x16: d.type = Type::P2SH;  d.network = Network::BitcoinMainnet; break; // DOGE Mainnet P2SH
                case 0x71: d.type = Type::P2PKH; d.network = Network::BitcoinTestnet; break; // DOGE Testnet P2PKH
                
                // DigiByte specific
                case 0x3f: d.type = Type::P2SH;  d.network = Network::BitcoinMainnet; break; // DGB Mainnet P2SH
                case 0x7e: d.type = Type::P2PKH; d.network = Network::BitcoinTestnet; break; // DGB Testnet P2PKH
                case 0x8c: d.type = Type::P2SH;  d.network = Network::BitcoinTestnet; break; // DGB Testnet P2SH
                
                default: return std::unexpected(Error::UnsupportedVersion);
            }
            return d;
        }
    }

    return std::unexpected(Error::UnknownPrefix);
}

// noexcept boundary. `addr` comes from an unauthenticated peer, and callers run
// inside asio handlers, which rethrow: a throw escaping here is std::terminate,
// not an error return. decode_impl() must never propagate.
std::expected<Decoded, Error> decode(std::string_view addr) noexcept {
    try {
        return decode_impl(addr);
    } catch (...) {
        return std::unexpected(Error::Internal);
    }
}

std::string script_pubkey_hex(const Decoded& d) {
    switch (d.type) {
        case Type::P2PKH:
        case Type::BchP2PKH:
            return "76a914" + to_hex(d.program) + "88ac";
        case Type::P2SH:
        case Type::BchP2SH:
            return "a914" + to_hex(d.program) + "87";
        case Type::P2WPKH:
            return "0014" + to_hex(d.program);
        case Type::P2WSH:
            return "0020" + to_hex(d.program);
        case Type::P2TR:
            return "5120" + to_hex(d.program);
    }
    return {};
}

std::expected<std::string, Error> decode_to_script_hex(std::string_view addr) {
    auto d = decode(addr);
    if (!d) return std::unexpected(d.error());
    return script_pubkey_hex(*d);
}

std::size_t output_vbytes(Type t) noexcept {
    switch (t) {
        case Type::P2PKH:    case Type::BchP2PKH: return 34;
        case Type::P2SH:     case Type::BchP2SH:  return 32;
        case Type::P2WPKH:                          return 31;
        case Type::P2WSH:                           return 43;
        case Type::P2TR:                            return 43;
    }
    return 0;
}

std::string_view error_to_string(Error e) noexcept {
    switch (e) {
        case Error::Empty:               return "empty";
        case Error::UnknownPrefix:       return "unknown prefix";
        case Error::BadLength:           return "bad length";
        case Error::BadChecksum:         return "bad checksum";
        case Error::BadCharacter:        return "bad character";
        case Error::UnsupportedVersion:  return "unsupported version";
        case Error::UnsupportedNetwork:  return "unsupported network";
        case Error::Internal:            return "internal decoder error";
    }
    return "unknown";
}

} // namespace mkpool::address
