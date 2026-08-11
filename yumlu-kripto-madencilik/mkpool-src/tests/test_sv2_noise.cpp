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
// File:        test_sv2_noise.cpp
// Description: Unit tests for the Stratum V2 Noise handshake/transport.
// Created:     2026-06-05
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include <catch2/catch_test_macros.hpp>
#include "sv2_noise.hpp"
#include <sodium.h>
#include <secp256k1.h>
#include <secp256k1_ellswift.h>
#include <cstring>
#include <stdexcept>
#include <iostream>

using mkpool::sv2::NoiseState;

// Simple HKDF-SHA256 implementation helper for the simulated client
static void client_hkdf_2(const uint8_t* salt, const uint8_t* ikm, size_t ikm_len, uint8_t* out1, uint8_t* out2) {
    uint8_t prk[32];
    crypto_auth_hmacsha256(prk, ikm, ikm_len, salt);

    uint8_t d1[1] = {0x01};
    crypto_auth_hmacsha256(out1, d1, 1, prk);

    uint8_t d2[33];
    std::memcpy(d2, out1, 32);
    d2[32] = 0x02;
    crypto_auth_hmacsha256(out2, d2, 33, prk);

    sodium_memzero(prk, sizeof(prk));
}

TEST_CASE("Noise NX Handshake and AEAD Transport End-to-End Verification", "[noise][crypto]") {
    // 0. Initialize sodium (just in case Catch2 main didn't, although mkpool does)
    if (sodium_init() < 0) {
        throw std::runtime_error("sodium_init failed");
    }

    // Server Static Authority Key (configured in config file). initialize()
    // takes the hex form directly, so there is nothing here to pre-decode.
    std::string authority_key_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    // 1. Initialize Server's NoiseState
    NoiseState server_state;
    REQUIRE(server_state.initialize(authority_key_hex));

    // 2. Simulated Client (Initiator) Initialization
    secp256k1_context* secp_ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    REQUIRE(secp_ctx != nullptr);

    // Ephemeral private key for client (e_priv_A)
    uint8_t e_priv_A[32];
    randombytes_buf(e_priv_A, 32);

    // Ephemeral public key for client (e_pub_A) in EllSwift format
    uint8_t e_pub_A[64];
    uint8_t aux[32];
    randombytes_buf(aux, 32);
    REQUIRE(secp256k1_ellswift_create(secp_ctx, e_pub_A, e_priv_A, aux) == 1);

    // Initialize Client Chaining Key (CK) and Hash State (H)
    uint8_t client_ck[32];
    uint8_t client_h[32];
    const char* proto_name = "Noise_NX_Secp256k1+EllSwift_ChaChaPoly_SHA256";
    // SV2 init: ck = SHA256(protocol_name), h = SHA256(ck) (double hash, matches NoiseState)
    crypto_hash_sha256(client_ck, reinterpret_cast<const uint8_t*>(proto_name), std::strlen(proto_name));
    crypto_hash_sha256(client_h, client_ck, 32);

    // Mix Client's e_pub_A into Client's Hash State (Act 1 message)
    {
        crypto_hash_sha256_state hs;
        crypto_hash_sha256_init(&hs);
        crypto_hash_sha256_update(&hs, client_h, 32);
        crypto_hash_sha256_update(&hs, e_pub_A, 64);
        crypto_hash_sha256_final(&hs, client_h);
    }

    // Mix the empty payload into Client's Hash State (corresponds to empty payload in Act 1)
    {
        crypto_hash_sha256_state hs;
        crypto_hash_sha256_init(&hs);
        crypto_hash_sha256_update(&hs, client_h, 32);
        crypto_hash_sha256_final(&hs, client_h);
    }

    // 3. Server processes Act 1
    REQUIRE(server_state.process_act1(e_pub_A));

    // 4. Server generates Act 2 response
    std::vector<uint8_t> act2 = server_state.generate_act2();
    REQUIRE(act2.size() == 234);
    REQUIRE(server_state.is_handshake_complete());

    // 5. Client processes Act 2 response
    // Format: e_pub_B (64) || c_s (80) || c_sig (90)
    uint8_t e_pub_B[64];
    uint8_t c_s[80];
    uint8_t c_sig[90];
    std::memcpy(e_pub_B, act2.data(), 64);
    std::memcpy(c_s, act2.data() + 64, 80);
    std::memcpy(c_sig, act2.data() + 144, 90);

    // Mix e_pub_B into Client's Hash State
    {
        crypto_hash_sha256_state hs;
        crypto_hash_sha256_init(&hs);
        crypto_hash_sha256_update(&hs, client_h, 32);
        crypto_hash_sha256_update(&hs, e_pub_B, 64);
        crypto_hash_sha256_final(&hs, client_h);
    }

    // Compute 'ee' shared secret on client side
    uint8_t ee_secret[32];
    REQUIRE(secp256k1_ellswift_xdh(secp_ctx, ee_secret, e_pub_A, e_pub_B, e_priv_A, 0, secp256k1_ellswift_xdh_hash_function_bip324, nullptr) == 1);

    // Derive temp_k1 on client side using HKDF-SHA256
    uint8_t temp_k1[32];
    client_hkdf_2(client_ck, ee_secret, 32, client_ck, temp_k1);

    // Decrypt c_s (Server's static public key s_pub_B) on client side
    uint8_t s_pub_B[64];
    unsigned long long decrypted_len = 0;
    uint8_t zero_nonce[12] = {0};
    REQUIRE(crypto_aead_chacha20poly1305_ietf_decrypt(
        s_pub_B, &decrypted_len,
        nullptr,
        c_s, 80,
        client_h, 32, // Associated data is current client hash state
        zero_nonce, temp_k1
    ) == 0);
    REQUIRE(decrypted_len == 64);

    // Mix c_s (ciphertext) into Client's Hash State
    {
        crypto_hash_sha256_state hs;
        crypto_hash_sha256_init(&hs);
        crypto_hash_sha256_update(&hs, client_h, 32);
        crypto_hash_sha256_update(&hs, c_s, 80);
        crypto_hash_sha256_final(&hs, client_h);
    }

    // Compute 'es' shared secret on client side
    uint8_t es_secret[32];
    REQUIRE(secp256k1_ellswift_xdh(secp_ctx, es_secret, e_pub_A, s_pub_B, e_priv_A, 0, secp256k1_ellswift_xdh_hash_function_bip324, nullptr) == 1);

    // Derive temp_k2 on client side using HKDF-SHA256
    uint8_t temp_k2[32];
    client_hkdf_2(client_ck, es_secret, 32, client_ck, temp_k2);

    // Decrypt c_sig (Server certificate) on client side
    // SignatureNoiseMessage = version(2) + valid_from(4) + not_valid_after(4) + sig(64) = 74 bytes
    uint8_t sig_msg[74];
    REQUIRE(crypto_aead_chacha20poly1305_ietf_decrypt(
        sig_msg, &decrypted_len,
        nullptr,
        c_sig, 90,
        client_h, 32, // Associated data is current client hash state
        zero_nonce, temp_k2  // Nonce is 0 because it was reset in mix_key
    ) == 0);
    REQUIRE(decrypted_len == 74);

    // Mix c_sig (ciphertext) into Client's Hash State
    {
        crypto_hash_sha256_state hs;
        crypto_hash_sha256_init(&hs);
        crypto_hash_sha256_update(&hs, client_h, 32);
        crypto_hash_sha256_update(&hs, c_sig, 90);
        crypto_hash_sha256_final(&hs, client_h);
    }

    // Derive client transport session keys:
    // client_tx_key (client to server) and client_rx_key (server to client)
    uint8_t client_tx_key[32];
    uint8_t client_rx_key[32];
    client_hkdf_2(client_ck, nullptr, 0, client_tx_key, client_rx_key);

    // Clean up secp256k1 context
    secp256k1_context_destroy(secp_ctx);

    // 6. Test Bidirectional Transport Encryption
    uint64_t client_tx_nonce = 0;
    uint64_t client_rx_nonce = 0;

    // Test Case A: Client -> Server Message
    std::string test_msg_client = "Hello from SV2 client!";
    size_t msg_len = test_msg_client.size();
    std::vector<uint8_t> client_ciphertext(msg_len + 16);

    // Client Encrypts:
    uint8_t client_nonce_buf[12] = {0};
    client_nonce_buf[4] = client_tx_nonce & 0xFF;
    client_nonce_buf[5] = (client_tx_nonce >> 8) & 0xFF;
    client_nonce_buf[6] = (client_tx_nonce >> 16) & 0xFF;
    client_nonce_buf[7] = (client_tx_nonce >> 24) & 0xFF;
    client_nonce_buf[8] = (client_tx_nonce >> 32) & 0xFF;
    client_nonce_buf[9] = (client_tx_nonce >> 40) & 0xFF;
    client_nonce_buf[10] = (client_tx_nonce >> 48) & 0xFF;
    client_nonce_buf[11] = (client_tx_nonce >> 56) & 0xFF;

    unsigned long long encrypted_len = 0;
    REQUIRE(crypto_aead_chacha20poly1305_ietf_encrypt(
        client_ciphertext.data(), &encrypted_len,
        reinterpret_cast<const uint8_t*>(test_msg_client.data()), msg_len,
        nullptr, 0,
        nullptr, client_nonce_buf, client_tx_key
    ) == 0);
    client_tx_nonce++;

    // Server Decrypts:
    std::vector<uint8_t> server_decrypted(msg_len);
    REQUIRE(server_state.decrypt(client_ciphertext.data(), client_ciphertext.size(), server_decrypted.data()));
    std::string decrypted_str(reinterpret_cast<const char*>(server_decrypted.data()), msg_len);
    CHECK(decrypted_str == test_msg_client);


    // Test Case B: Server -> Client Message
    std::string test_msg_server = "Welcome to mkpool Stratum V2!";
    size_t server_msg_len = test_msg_server.size();
    std::vector<uint8_t> server_ciphertext(server_msg_len + 16);

    // Server Encrypts:
    REQUIRE(server_state.encrypt(reinterpret_cast<const uint8_t*>(test_msg_server.data()), server_msg_len, server_ciphertext.data()));

    // Client Decrypts:
    uint8_t server_nonce_buf[12] = {0};
    server_nonce_buf[4] = client_rx_nonce & 0xFF;
    server_nonce_buf[5] = (client_rx_nonce >> 8) & 0xFF;
    server_nonce_buf[6] = (client_rx_nonce >> 16) & 0xFF;
    server_nonce_buf[7] = (client_rx_nonce >> 24) & 0xFF;
    server_nonce_buf[8] = (client_rx_nonce >> 32) & 0xFF;
    server_nonce_buf[9] = (client_rx_nonce >> 40) & 0xFF;
    server_nonce_buf[10] = (client_rx_nonce >> 48) & 0xFF;
    server_nonce_buf[11] = (client_rx_nonce >> 56) & 0xFF;

    std::vector<uint8_t> client_decrypted(server_msg_len);
    REQUIRE(crypto_aead_chacha20poly1305_ietf_decrypt(
        client_decrypted.data(), &decrypted_len,
        nullptr,
        server_ciphertext.data(), server_ciphertext.size(),
        nullptr, 0,
        server_nonce_buf, client_rx_key
    ) == 0);
    client_rx_nonce++;

    std::string server_decrypted_str(reinterpret_cast<const char*>(client_decrypted.data()), server_msg_len);
    CHECK(server_decrypted_str == test_msg_server);
}
