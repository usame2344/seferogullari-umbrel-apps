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
// File:        sv2_noise.cpp
// Description: Stratum V2 Noise (NX) handshake and AEAD transport encryption.
// Created:     2026-05-27
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "sv2_noise.hpp"
#include <sodium.h>
#include <secp256k1.h>
#include <secp256k1_ellswift.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
#include <stdexcept>
#include <cstring>
#include <chrono>
#include <spdlog/spdlog.h>

namespace mkpool::sv2 {

// Helper to convert hex to binary bytes
static bool hex_to_bytes(const std::string& hex, uint8_t* out, size_t out_len) {
    if (hex.length() != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        std::string byteString = hex.substr(i * 2, 2);
        char* end;
        out[i] = static_cast<uint8_t>(std::strtol(byteString.c_str(), &end, 16));
        if (*end != '\0') return false;
    }
    return true;
}

NoiseState::NoiseState() {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    secp_ctx_ = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!secp_ctx_) {
        throw std::runtime_error("Failed to create secp256k1 context");
    }
    std::memset(ck_, 0, sizeof(ck_));
    std::memset(h_, 0, sizeof(h_));
    std::memset(rx_key_, 0, sizeof(rx_key_));
    std::memset(tx_key_, 0, sizeof(tx_key_));
    std::memset(authority_priv_, 0, sizeof(authority_priv_));
    std::memset(s_priv_B_, 0, sizeof(s_priv_B_));
    std::memset(s_pub_B_, 0, sizeof(s_pub_B_));
    std::memset(e_priv_B_, 0, sizeof(e_priv_B_));
    std::memset(e_pub_B_, 0, sizeof(e_pub_B_));
    std::memset(e_pub_A_, 0, sizeof(e_pub_A_));
}

NoiseState::~NoiseState() {
    if (secp_ctx_) {
        secp256k1_context_destroy(secp_ctx_);
    }
    // Securely clear all private cryptographic parameters
    sodium_memzero(authority_priv_, sizeof(authority_priv_));
    sodium_memzero(s_priv_B_, sizeof(s_priv_B_));
    sodium_memzero(e_priv_B_, sizeof(e_priv_B_));
    sodium_memzero(rx_key_, sizeof(rx_key_));
    sodium_memzero(tx_key_, sizeof(tx_key_));
    sodium_memzero(ck_, sizeof(ck_));
}

NoiseState::NoiseState(NoiseState&& other) noexcept {
    secp_ctx_ = other.secp_ctx_;
    other.secp_ctx_ = nullptr;
    handshake_complete_ = other.handshake_complete_;
    std::memcpy(ck_, other.ck_, 32);
    std::memcpy(h_, other.h_, 32);
    std::memcpy(rx_key_, other.rx_key_, 32);
    std::memcpy(tx_key_, other.tx_key_, 32);
    rx_nonce_ = other.rx_nonce_;
    tx_nonce_ = other.tx_nonce_;
    std::memcpy(s_priv_B_, other.s_priv_B_, 32);
    std::memcpy(s_pub_B_, other.s_pub_B_, 64);
    std::memcpy(e_priv_B_, other.e_priv_B_, 32);
    std::memcpy(e_pub_B_, other.e_pub_B_, 64);
    std::memcpy(e_pub_A_, other.e_pub_A_, 64);
}

NoiseState& NoiseState::operator=(NoiseState&& other) noexcept {
    if (this != &other) {
        if (secp_ctx_) secp256k1_context_destroy(secp_ctx_);
        secp_ctx_ = other.secp_ctx_;
        other.secp_ctx_ = nullptr;
        handshake_complete_ = other.handshake_complete_;
        std::memcpy(ck_, other.ck_, 32);
        std::memcpy(h_, other.h_, 32);
        std::memcpy(rx_key_, other.rx_key_, 32);
        std::memcpy(tx_key_, other.tx_key_, 32);
        rx_nonce_ = other.rx_nonce_;
        tx_nonce_ = other.tx_nonce_;
        std::memcpy(s_priv_B_, other.s_priv_B_, 32);
        std::memcpy(s_pub_B_, other.s_pub_B_, 64);
        std::memcpy(e_priv_B_, other.e_priv_B_, 32);
        std::memcpy(e_pub_B_, other.e_pub_B_, 64);
        std::memcpy(e_pub_A_, other.e_pub_A_, 64);
    }
    return *this;
}

bool NoiseState::initialize(const std::string& authority_key_hex) {
    if (!hex_to_bytes(authority_key_hex, authority_priv_, 32)) {
        spdlog::error("[NoiseState] Invalid authority key hex string (must be 64 characters hex)");
        return false;
    }

    // Generate a fresh static keypair for this session, ensuring even parity
    // (SRI convention: negate secret key if public key has odd y-coordinate)
    for (;;) {
        randombytes_buf(s_priv_B_, 32);
        secp256k1_pubkey pub_test;
        if (!secp256k1_ec_pubkey_create(secp_ctx_, &pub_test, s_priv_B_)) continue;
        secp256k1_xonly_pubkey xonly_test;
        int parity = 0;
        secp256k1_xonly_pubkey_from_pubkey(secp_ctx_, &xonly_test, &parity, &pub_test);
        if (parity == 1) {
            // Negate the key to get even parity. Returns 0 only for a key that
            // is already invalid, and zeroes it when it does -- so an unchecked
            // failure here would hand a zeroed static key to the handshake.
            // pubkey_create above already validated it, hence unreachable in
            // practice; retry rather than assume.
            if (!secp256k1_ec_seckey_negate(secp_ctx_, s_priv_B_)) continue;
        }
        break;
    }

    // Construct our EllSwift static public key from the fresh static private key
    uint8_t aux[32];
    randombytes_buf(aux, 32);
    if (!secp256k1_ellswift_create(secp_ctx_, s_pub_B_, s_priv_B_, aux)) {
        spdlog::error("[NoiseState] Failed to derive EllSwift static public key");
        return false;
    }

    // Setup Noise Handshake CK and H constants
    // SRI convention: ck = SHA256(protocol_name), h = SHA256(ck)
    const char* proto_name = "Noise_NX_Secp256k1+EllSwift_ChaChaPoly_SHA256";
    crypto_hash_sha256(ck_, reinterpret_cast<const uint8_t*>(proto_name), std::strlen(proto_name));
    crypto_hash_sha256(h_, ck_, 32);

    return true;
}

void NoiseState::mix_hash(const uint8_t* data, size_t len) {
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    crypto_hash_sha256_update(&state, h_, 32);
    if (len > 0 && data != nullptr) {
        crypto_hash_sha256_update(&state, data, len);
    }
    crypto_hash_sha256_final(&state, h_);
}

void NoiseState::mix_key(const uint8_t* input_key_material, uint8_t* out_key) {
    uint8_t prk[32];
    // prk = HMAC-SHA256(key = ck_, data = input_key_material)
    crypto_auth_hmacsha256(prk, input_key_material, 32, ck_);

    // ck_ = HMAC-SHA256(key = prk, data = 0x01)
    uint8_t data1[1] = {0x01};
    crypto_auth_hmacsha256(ck_, data1, 1, prk);

    // out_key = HMAC-SHA256(key = prk, data = ck_ || 0x02)
    uint8_t data2[33];
    std::memcpy(data2, ck_, 32);
    data2[32] = 0x02;
    crypto_auth_hmacsha256(out_key, data2, 33, prk);

    sodium_memzero(prk, sizeof(prk));
}

bool NoiseState::process_act1(const uint8_t* client_ellswift) {
    std::memcpy(e_pub_A_, client_ellswift, 64);
    mix_hash(e_pub_A_, 64);
    // Mix the empty payload ciphertext into the hash state as required by Noise_NX spec
    mix_hash(nullptr, 0);
    return true;
}

std::vector<uint8_t> NoiseState::generate_act2() {
    // Generate secure random ephemeral private key with even parity (SRI convention)
    for (;;) {
        randombytes_buf(e_priv_B_, 32);
        secp256k1_pubkey pub_test;
        if (!secp256k1_ec_pubkey_create(secp_ctx_, &pub_test, e_priv_B_)) continue;
        secp256k1_xonly_pubkey xonly_test;
        int parity = 0;
        secp256k1_xonly_pubkey_from_pubkey(secp_ctx_, &xonly_test, &parity, &pub_test);
        if (parity == 1) {
            // See the static-key loop above: negate only fails on an already
            // invalid key, which it also zeroes. Retry instead of proceeding.
            if (!secp256k1_ec_seckey_negate(secp_ctx_, e_priv_B_)) continue;
        }
        break;
    }
    
    // Create EllSwift ephemeral public key e_pub_B_
    uint8_t aux[32];
    randombytes_buf(aux, 32);
    if (!secp256k1_ellswift_create(secp_ctx_, e_pub_B_, e_priv_B_, aux)) {
        throw std::runtime_error("Failed to construct server ephemeral EllSwift key");
    }

    mix_hash(e_pub_B_, 64);

    // 1. Perform ee ECDH exchange
    uint8_t ee_secret[32];
    if (!secp256k1_ellswift_xdh(secp_ctx_, ee_secret, e_pub_A_, e_pub_B_, e_priv_B_, 1, secp256k1_ellswift_xdh_hash_function_bip324, nullptr)) {
        throw std::runtime_error("secp256k1_ellswift_xdh for ee failed");
    }
    
    uint8_t temp_k1[32];
    mix_key(ee_secret, temp_k1);

    sodium_memzero(ee_secret, sizeof(ee_secret));

    // 2. Encrypt our static public key s_pub_B_ (64 bytes -> 80 bytes with MAC)
    uint8_t c_s[80];
    uint8_t nonce[12] = {0}; // Nonce is 0 for s_B encryption
    unsigned long long ciphertext_len = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(
        c_s, &ciphertext_len,
        s_pub_B_, 64,
        h_, 32, // Associated data is the current Hash State H
        nullptr, nonce, temp_k1
    );
    mix_hash(c_s, 80);

    // 3. Perform es ECDH exchange
    uint8_t es_secret[32];
    if (!secp256k1_ellswift_xdh(secp_ctx_, es_secret, e_pub_A_, s_pub_B_, s_priv_B_, 1, secp256k1_ellswift_xdh_hash_function_bip324, nullptr)) {
        throw std::runtime_error("secp256k1_ellswift_xdh for es failed");
    }

    uint8_t temp_k2[32];
    mix_key(es_secret, temp_k2);
    sodium_memzero(es_secret, sizeof(es_secret));

    // 4. Construct SV2 Certificate (SignatureNoiseMessage)
    // Spec format: version(u16 LE) + valid_from(u32 LE) + not_valid_after(u32 LE) + schnorr_sig(64B) = 74 bytes
    uint8_t sig_msg[74];
    std::memset(sig_msg, 0, sizeof(sig_msg));
    
    auto now = std::chrono::system_clock::now();
    uint32_t now_s = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
    // Backdate valid_from by 1 hour so ordinary client/server clock skew does not make the
    // certificate appear "not yet valid". SRI strictly enforces now >= valid_from (ESP-Miner
    // / bitaxe does NOT, which is why it never tripped on a valid_from == now cert). A purely
    // self-issued cert (we own the authority key) gains nothing from a tight start time, so a
    // generous backdate is safe and avoids skew-induced handshake failures.
    uint32_t valid_from = now_s - 3600;
    uint32_t not_valid_after = now_s + (365 * 24 * 3600); // ~1 year validity

    // version = 0 (u16 LE)
    sig_msg[0] = 0x00;
    sig_msg[1] = 0x00;

    // valid_from (u32 LE)
    sig_msg[2] = (valid_from >>  0) & 0xFF;
    sig_msg[3] = (valid_from >>  8) & 0xFF;
    sig_msg[4] = (valid_from >> 16) & 0xFF;
    sig_msg[5] = (valid_from >> 24) & 0xFF;

    // not_valid_after (u32 LE)
    sig_msg[6] = (not_valid_after >>  0) & 0xFF;
    sig_msg[7] = (not_valid_after >>  8) & 0xFF;
    sig_msg[8] = (not_valid_after >> 16) & 0xFF;
    sig_msg[9] = (not_valid_after >> 24) & 0xFF;

    // Generate Schnorr BIP340 Signature
    // Message to sign = SHA256(version(2) + valid_from(4) + not_valid_after(4) + xonly_pubkey(32))
    secp256k1_pubkey full_pubkey;
    if (!secp256k1_ellswift_decode(secp_ctx_, &full_pubkey, s_pub_B_)) {
        throw std::runtime_error("Failed to decode server static EllSwift public key");
    }

    secp256k1_xonly_pubkey xonly_pubkey;
    int parity = 0;
    if (!secp256k1_xonly_pubkey_from_pubkey(secp_ctx_, &xonly_pubkey, &parity, &full_pubkey)) {
        throw std::runtime_error("Failed to obtain x-only public key");
    }

    uint8_t serialized_xonly[32];
    secp256k1_xonly_pubkey_serialize(secp_ctx_, serialized_xonly, &xonly_pubkey);

    // Hash: SHA256(version || valid_from || not_valid_after || xonly_pubkey)
    uint8_t msg_hash[32];
    {
        crypto_hash_sha256_state state;
        crypto_hash_sha256_init(&state);
        crypto_hash_sha256_update(&state, sig_msg, 10);  // version(2) + valid_from(4) + not_valid_after(4)
        crypto_hash_sha256_update(&state, serialized_xonly, 32);
        crypto_hash_sha256_final(&state, msg_hash);
    }

    // Sign with the AUTHORITY private key (not the static key)
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(secp_ctx_, &keypair, authority_priv_)) {
        throw std::runtime_error("Failed to create secp256k1 keypair from authority private key");
    }

    uint8_t schnorr_sig[64];
    uint8_t sig_aux[32];
    randombytes_buf(sig_aux, 32);
    if (!secp256k1_schnorrsig_sign32(secp_ctx_, schnorr_sig, msg_hash, &keypair, sig_aux)) {
        throw std::runtime_error("Schnorr BIP340 signature creation failed");
    }

    std::memcpy(&sig_msg[10], schnorr_sig, 64);

    // Encrypt the certificate (74 bytes) -> c_sig (90 bytes = 74 + 16 MAC)
    uint8_t c_sig[90];
    std::memset(nonce, 0, sizeof(nonce)); // Nonce = 0 for signature message encryption
    crypto_aead_chacha20poly1305_ietf_encrypt(
        c_sig, &ciphertext_len,
        sig_msg, 74,
        h_, 32, // Associated data is the current Hash State H
        nullptr, nonce, temp_k2
    );
    mix_hash(c_sig, 90);

    // 5. Split: HKDF-2(ck, zerolen) -> (c1=rx_key, c2=tx_key)
    // As responder: c1 = initiator->responder = our rx_key, c2 = our tx_key
    uint8_t prk[32];
    // HMAC-SHA256(key=ck, data=empty)
    uint8_t empty_buf = 0;
    crypto_auth_hmacsha256(prk, &empty_buf, 0, ck_);

    uint8_t d1[1] = {0x01};
    crypto_auth_hmacsha256(rx_key_, d1, 1, prk);

    uint8_t d2[33];
    std::memcpy(d2, rx_key_, 32);
    d2[32] = 0x02;
    crypto_auth_hmacsha256(tx_key_, d2, 33, prk);

    sodium_memzero(prk, sizeof(prk));
    sodium_memzero(temp_k1, sizeof(temp_k1));
    sodium_memzero(temp_k2, sizeof(temp_k2));

    // Consolidate response payload: e_pub_B (64) || c_s (80) || c_sig (90) = 234 bytes
    std::vector<uint8_t> response;
    response.reserve(234);
    response.insert(response.end(), e_pub_B_, e_pub_B_ + 64);
    response.insert(response.end(), c_s, c_s + 80);
    response.insert(response.end(), c_sig, c_sig + 90);

    rx_nonce_ = 0;
    tx_nonce_ = 0;
    handshake_complete_ = true;

    spdlog::info("[NoiseState] Handshake completed successfully, derived transport session keys");
    return response;
}

bool NoiseState::decrypt(const uint8_t* ciphertext, size_t len, uint8_t* out_plaintext) {
    if (len < 16) return false;
    
    uint8_t nonce[12] = {0};
    // Correct little-endian transport nonce serialization mapping:
    // First 4 bytes are zero, followed by 8 bytes of the little-endian 64-bit counter.
    nonce[4] = rx_nonce_ & 0xFF;
    nonce[5] = (rx_nonce_ >> 8) & 0xFF;
    nonce[6] = (rx_nonce_ >> 16) & 0xFF;
    nonce[7] = (rx_nonce_ >> 24) & 0xFF;
    nonce[8] = (rx_nonce_ >> 32) & 0xFF;
    nonce[9] = (rx_nonce_ >> 40) & 0xFF;
    nonce[10] = (rx_nonce_ >> 48) & 0xFF;
    nonce[11] = (rx_nonce_ >> 56) & 0xFF;

    unsigned long long decrypted_len = 0;
    int res = crypto_aead_chacha20poly1305_ietf_decrypt(
        out_plaintext, &decrypted_len,
        nullptr,
        ciphertext, len,
        nullptr, 0, // No associated data for transport packets
        nonce, rx_key_
    );

    if (res == 0) {
        rx_nonce_++;
        return true;
    }
    return false;
}

bool NoiseState::encrypt(const uint8_t* plaintext, size_t len, uint8_t* out_ciphertext) {
    uint8_t nonce[12] = {0};
    // Correct little-endian transport nonce serialization mapping:
    // First 4 bytes are zero, followed by 8 bytes of the little-endian 64-bit counter.
    nonce[4] = tx_nonce_ & 0xFF;
    nonce[5] = (tx_nonce_ >> 8) & 0xFF;
    nonce[6] = (tx_nonce_ >> 16) & 0xFF;
    nonce[7] = (tx_nonce_ >> 24) & 0xFF;
    nonce[8] = (tx_nonce_ >> 32) & 0xFF;
    nonce[9] = (tx_nonce_ >> 40) & 0xFF;
    nonce[10] = (tx_nonce_ >> 48) & 0xFF;
    nonce[11] = (tx_nonce_ >> 56) & 0xFF;

    unsigned long long encrypted_len = 0;
    int res = crypto_aead_chacha20poly1305_ietf_encrypt(
        out_ciphertext, &encrypted_len,
        plaintext, len,
        nullptr, 0, // No associated data for transport packets
        nullptr, nonce, tx_key_
    );

    if (res == 0) {
        tx_nonce_++;
        return true;
    }
    return false;
}

} // namespace mkpool::sv2
