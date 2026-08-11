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
// File:        sv2_noise.hpp
// Description: Stratum V2 Noise handshake/transport interface.
// Created:     2026-05-27
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <secp256k1.h>
#include <secp256k1_ellswift.h>

namespace mkpool::sv2 {

class NoiseState {
public:
    NoiseState();
    ~NoiseState();

    // Prevent copying
    NoiseState(const NoiseState&) = delete;
    NoiseState& operator=(const NoiseState&) = delete;

    // Move constructible
    NoiseState(NoiseState&& other) noexcept;
    NoiseState& operator=(NoiseState&& other) noexcept;

    /**
     * @brief Initialize the Noise State with the server's long-term authority static private key.
     * @param authority_key_hex Hexadecimal representation of the pool's static private key (32 bytes).
     * @return true if initialization succeeded, false otherwise.
     */
    bool initialize(const std::string& authority_key_hex);

    /**
     * @brief Process the client's Noise Handshake Act 1 payload (64 bytes EllSwift ephemeral key).
     * @param client_ellswift Pointer to the 64-byte EllSwift key received from the client.
     * @return true if successful, false otherwise.
     */
    bool process_act1(const uint8_t* client_ellswift);

    /**
     * @brief Generate the server's Act 2 handshake response.
     *        This performs the ee and es ECDH operations, encrypts the server's static public key and
     *        the authentication certificate, and derives the rx and tx session keys.
     * @return The 234-byte serialized Act 2 payload.
     */
    std::vector<uint8_t> generate_act2();

    /**
     * @brief Decrypt incoming encrypted data.
     * @param ciphertext Pointer to the ciphertext payload.
     * @param len Length of the ciphertext including the 16-byte MAC tag.
     * @param out_plaintext Pointer to output buffer (must be at least len - 16 bytes).
     * @return true if decryption and MAC validation succeeded, false otherwise.
     */
    bool decrypt(const uint8_t* ciphertext, size_t len, uint8_t* out_plaintext);

    /**
     * @brief Encrypt outgoing data.
     * @param plaintext Pointer to the plaintext payload.
     * @param len Length of the plaintext.
     * @param out_ciphertext Pointer to output buffer (must be at least len + 16 bytes).
     * @return true if encryption succeeded, false otherwise.
     */
    bool encrypt(const uint8_t* plaintext, size_t len, uint8_t* out_ciphertext);

    bool is_handshake_complete() const { return handshake_complete_; }

private:
    secp256k1_context* secp_ctx_;
    bool handshake_complete_ = false;

    // Noise Handshake Protocol Chaining Key & Hash State
    uint8_t ck_[32];
    uint8_t h_[32];

    // Transport Encryption Session Keys and Nonces
    uint8_t rx_key_[32];
    uint8_t tx_key_[32];
    uint64_t rx_nonce_ = 0;
    uint64_t tx_nonce_ = 0;

    // Keys
    uint8_t authority_priv_[32]; // Authority Private Key (signs certificates)
    uint8_t s_priv_B_[32]; // Server Static Private Key (generated fresh each init)
    uint8_t s_pub_B_[64];  // Server Static Public Key (EllSwift)
    uint8_t e_priv_B_[32]; // Server Ephemeral Private Key
    uint8_t e_pub_B_[64];  // Server Ephemeral Public Key (EllSwift)
    uint8_t e_pub_A_[64];  // Client Ephemeral Public Key (EllSwift)

    // Helper functions
    void mix_hash(const uint8_t* data, size_t len);
    void mix_key(const uint8_t* input_key_material, uint8_t* out_key);
};

} // namespace mkpool::sv2
