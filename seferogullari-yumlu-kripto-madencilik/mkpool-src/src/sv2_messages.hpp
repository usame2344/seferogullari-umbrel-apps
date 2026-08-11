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
// File:        sv2_messages.hpp
// Description: Stratum V2 message type definitions.
// Created:     2026-05-27
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include "sv2_codec.hpp"
#include <vector>
#include <string>
#include <array>
#include <optional>
#include <cstdint>

namespace mkpool::sv2 {

// Extension / Msg Types
constexpr uint16_t EXT_COMMON = 0x0000;

constexpr uint8_t MSG_SETUP_CONNECTION         = 0x00;
constexpr uint8_t MSG_SETUP_CONNECTION_SUCCESS = 0x01;
constexpr uint8_t MSG_SETUP_CONNECTION_ERROR   = 0x02;

constexpr uint8_t MSG_OPEN_STANDARD_MINING_CHANNEL         = 0x10;
constexpr uint8_t MSG_OPEN_STANDARD_MINING_CHANNEL_SUCCESS = 0x11;
constexpr uint8_t MSG_OPEN_MINING_CHANNEL_ERROR            = 0x12;
constexpr uint8_t MSG_OPEN_EXTENDED_MINING_CHANNEL         = 0x13;
constexpr uint8_t MSG_OPEN_EXTENDED_MINING_CHANNEL_SUCCESS = 0x14;
constexpr uint8_t MSG_UPDATE_CHANNEL                       = 0x16;
constexpr uint8_t MSG_UPDATE_CHANNEL_ERROR                 = 0x17;
constexpr uint8_t MSG_CLOSE_CHANNEL                        = 0x18;
constexpr uint8_t MSG_SET_EXTRANONCE_PREFIX                = 0x19;

constexpr uint8_t MSG_NEW_MINING_JOB        = 0x15;
constexpr uint8_t MSG_NEW_EXTENDED_MINING_JOB = 0x1f;
constexpr uint8_t MSG_SUBMIT_SHARES_STANDARD = 0x1a;
constexpr uint8_t MSG_SUBMIT_SHARES_EXTENDED = 0x1b;
constexpr uint8_t MSG_SUBMIT_SHARES_SUCCESS  = 0x1c;
constexpr uint8_t MSG_SUBMIT_SHARES_ERROR    = 0x1d;

constexpr uint8_t MSG_SET_NEW_PREV_HASH = 0x20;
constexpr uint8_t MSG_SET_TARGET        = 0x21;

constexpr uint8_t MSG_SET_CUSTOM_MINING_JOB         = 0x22;
constexpr uint8_t MSG_SET_CUSTOM_MINING_JOB_SUCCESS = 0x23;
constexpr uint8_t MSG_SET_CUSTOM_MINING_JOB_ERROR   = 0x24;

// Most-significant bit of extension_type. Per the SV2 spec (03-Protocol-Overview),
// channel-scoped messages (those whose first payload field is channel_id) MUST set this
// bit; the receiver is required to IGNORE it for extension lookup (so 0x8ABC == 0x0ABC).
// We were never setting it, which a lenient client (ESP-Miner) tolerates but a strict
// codec (Braiins) rejects, dropping our mining-job/set-target/share-result messages.
constexpr uint16_t CHANNEL_MSG_BIT = 0x8000;

// True for message types that are channel-scoped (channel_id is the first payload field),
// i.e. the channel_msg bit must be set in extension_type when framing them.
constexpr bool is_channel_msg(uint8_t msg_type) {
    switch (msg_type) {
        case MSG_UPDATE_CHANNEL:          // 0x16
        case MSG_UPDATE_CHANNEL_ERROR:    // 0x17
        case MSG_CLOSE_CHANNEL:           // 0x18
        case MSG_SET_EXTRANONCE_PREFIX:   // 0x19
        case MSG_SUBMIT_SHARES_STANDARD:  // 0x1a
        case MSG_SUBMIT_SHARES_EXTENDED:  // 0x1b
        case MSG_SUBMIT_SHARES_SUCCESS:   // 0x1c
        case MSG_SUBMIT_SHARES_ERROR:     // 0x1d
        case MSG_NEW_MINING_JOB:          // standard-channel job
        case MSG_NEW_EXTENDED_MINING_JOB: // 0x1f
        case MSG_SET_NEW_PREV_HASH:       // 0x20
        case MSG_SET_TARGET:              // 0x21
            return true;
        default:
            return false; // SetupConnection*, Open*Channel(Success/Error), SetCustomMiningJob* are NOT channel-scoped
    }
}

// Flags for SetupConnection (client -> server request)
constexpr uint32_t FLAG_REQUIRES_STANDARD_JOBS   = 1 << 0;
constexpr uint32_t FLAG_REQUIRES_WORK_SELECTION  = 1 << 1;
constexpr uint32_t FLAG_REQUIRES_VERSION_ROLLING = 1 << 2;

// Flags for SetupConnection.Success (server -> client response). These are a
// DIFFERENT namespace from the request flags above: here bit 0 means the server
// forbids version rolling, not "standard jobs". We allow version rolling and do
// not require extended channels, so Success.flags MUST be 0.
constexpr uint32_t SUCCESS_FLAG_REQUIRES_FIXED_VERSION    = 1 << 0;
constexpr uint32_t SUCCESS_FLAG_REQUIRES_EXTENDED_CHANNELS = 1 << 1;

// ----------------------------------------------------------------------------
// Framing Header
// ----------------------------------------------------------------------------

// Largest payload a single frame may declare. A Noise transport message is
// capped at 65535 bytes including its 16-byte MAC, and an SV2 payload is
// decrypted as exactly one such message, so no legitimate frame can exceed
// this. The wire field is 24 bits (up to 16 MiB), which is why the bound has to
// be enforced by the reader rather than inferred from the type: an unchecked
// msg_length is a peer naming the byte count we go on to read and allocate.
constexpr uint32_t MAX_MSG_LENGTH = 65535 - 16; // 65519

// Single source of truth for "is this declared frame length sane?". Every place
// that acts on a peer-supplied msg_length must gate on this, so the rule can be
// tested and fuzzed directly instead of being re-typed per call site (a
// duplicated bound is a bound that drifts).
[[nodiscard]] constexpr bool frame_length_ok(uint32_t msg_length) noexcept {
    return msg_length <= MAX_MSG_LENGTH;
}

struct Header {
    uint16_t extension_type{0};
    uint8_t  msg_type{0};
    uint32_t msg_length{0}; // 24-bit

    void serialize(Writer& w) const {
        w.write_u16(extension_type);
        w.write_u8(msg_type);
        w.write_u24(msg_length);
    }

    void deserialize(Reader& r) {
        extension_type = r.read_u16();
        msg_type       = r.read_u8();
        msg_length     = r.read_u24();
    }
};

// ----------------------------------------------------------------------------
// SetupConnection (0x00) - Client to Server
// ----------------------------------------------------------------------------
struct SetupConnection {
    uint8_t      protocol{0}; // 0 = Mining, 2 = JD
    uint16_t     min_version{2};
    uint16_t     max_version{2};
    uint32_t     flags{0};
    std::string  endpoint_host;
    uint16_t     endpoint_port{0};
    std::string  vendor;
    std::string  hardware_version;
    std::string  firmware;
    std::string  device_id;

    void deserialize(Reader& r) {
        protocol         = r.read_u8();
        min_version      = r.read_u16();
        max_version      = r.read_u16();
        flags            = r.read_u32();
        endpoint_host    = r.read_str0_255();
        endpoint_port    = r.read_u16();
        vendor           = r.read_str0_255();
        hardware_version = r.read_str0_255();
        firmware         = r.read_str0_255();
        device_id        = r.read_str0_255();
    }
};

// ----------------------------------------------------------------------------
// SetupConnection.Success (0x01) - Server to Client
// ----------------------------------------------------------------------------
struct SetupConnectionSuccess {
    uint16_t used_version{2};
    uint32_t flags{0};

    void serialize(Writer& w) const {
        w.write_u16(used_version);
        w.write_u32(flags);
    }
};

// ----------------------------------------------------------------------------
// SetupConnection.Error (0x02) - Server to Client
// ----------------------------------------------------------------------------
struct SetupConnectionError {
    std::string error_code;
    uint32_t    flags{0};

    void serialize(Writer& w) const {
        w.write_str0_255(error_code);
        w.write_u32(flags);
    }
};

// ----------------------------------------------------------------------------
// OpenStandardMiningChannel (0x10) - Client to Server
// ----------------------------------------------------------------------------
struct OpenStandardMiningChannel {
    uint32_t                request_id{0};
    std::string             user_identity;
    float                   nominal_hash_rate{0.0f};
    std::array<uint8_t, 32> max_target{};

    void deserialize(Reader& r) {
        request_id        = r.read_u32();
        user_identity     = r.read_str0_255();
        nominal_hash_rate = r.read_f32();
        max_target        = r.read_u256();
    }
};

// ----------------------------------------------------------------------------
// OpenStandardMiningChannel.Success (0x11) - Server to Client
// ----------------------------------------------------------------------------
struct OpenStandardMiningChannelSuccess {
    uint32_t                request_id{0};
    uint32_t                channel_id{0};
    std::array<uint8_t, 32> target{};
    std::vector<uint8_t>    extranonce_prefix;
    uint32_t                group_channel_id{0};

    void serialize(Writer& w) const {
        w.write_u32(request_id);
        w.write_u32(channel_id);
        w.write_u256(target);
        w.write_b0_32(extranonce_prefix);
        w.write_u32(group_channel_id);
    }
};

// ----------------------------------------------------------------------------
// OpenMiningChannel.Error (0x12) - Server to Client
// ----------------------------------------------------------------------------
struct OpenMiningChannelError {
    uint32_t    request_id{0};
    std::string error_code;

    void serialize(Writer& w) const {
        w.write_u32(request_id);
        w.write_str0_255(error_code);
    }
};

// ----------------------------------------------------------------------------
// OpenExtendedMiningChannel (0x13) - Client to Server
// ----------------------------------------------------------------------------
struct OpenExtendedMiningChannel {
    uint32_t                request_id{0};
    std::string             user_identity;
    float                   nominal_hash_rate{0.0f};
    std::array<uint8_t, 32> max_target{};
    uint16_t                min_extranonce_size{0};

    void deserialize(Reader& r) {
        request_id          = r.read_u32();
        user_identity       = r.read_str0_255();
        nominal_hash_rate   = r.read_f32();
        max_target          = r.read_u256();
        min_extranonce_size = r.read_u16();
    }
};

// ----------------------------------------------------------------------------
// OpenExtendedMiningChannel.Success (0x14) - Server to Client
// ----------------------------------------------------------------------------
struct OpenExtendedMiningChannelSuccess {
    uint32_t                request_id{0};
    uint32_t                channel_id{0};
    std::array<uint8_t, 32> target{};
    uint16_t                extranonce_size{0};
    std::vector<uint8_t>    extranonce_prefix;
    uint32_t                group_channel_id{0};

    void serialize(Writer& w) const {
        w.write_u32(request_id);
        w.write_u32(channel_id);
        w.write_u256(target);
        w.write_u16(extranonce_size);
        w.write_b0_32(extranonce_prefix);
        w.write_u32(group_channel_id);
    }
};

// ----------------------------------------------------------------------------
// NewExtendedMiningJob (0x1f) - Server to Client
// ----------------------------------------------------------------------------
struct NewExtendedMiningJob {
    uint32_t                             channel_id{0};
    uint32_t                             job_id{0};
    std::optional<uint32_t>              min_ntime;
    uint32_t                             version{0};
    bool                                 version_rolling_allowed{false};
    std::vector<std::array<uint8_t, 32>> merkle_path;
    std::vector<uint8_t>                 coinbase_tx_prefix;
    std::vector<uint8_t>                 coinbase_tx_suffix;

    void serialize(Writer& w) const {
        w.write_u32(channel_id);
        w.write_u32(job_id);
        w.write_option_u32(min_ntime);
        w.write_u32(version);
        w.write_bool(version_rolling_allowed);
        w.write_seq0_255_u256(merkle_path);
        w.write_b0_64k(coinbase_tx_prefix);
        w.write_b0_64k(coinbase_tx_suffix);
    }
};

// ----------------------------------------------------------------------------
// UpdateChannel (0x16) - Client to Server
// ----------------------------------------------------------------------------
struct UpdateChannel {
    uint32_t                channel_id{0};
    float                   nominal_hash_rate{0.0f};
    std::array<uint8_t, 32> maximum_target{};

    void deserialize(Reader& r) {
        channel_id        = r.read_u32();
        nominal_hash_rate = r.read_f32();
        maximum_target    = r.read_u256();
    }
};

// ----------------------------------------------------------------------------
// CloseChannel (0x18) - Client <-> Server (bidirectional)
// ----------------------------------------------------------------------------
// Per SV2 spec (sv2-spec/05-Mining-Protocol): the sender closes a single
// channel; the receiver MUST stop sending messages for that channel. There is
// NO success/response message defined for CloseChannel.
struct CloseChannel {
    uint32_t    channel_id{0};
    std::string reason_code;

    void deserialize(Reader& r) {
        channel_id  = r.read_u32();
        reason_code = r.read_str0_255();
    }
};

// ----------------------------------------------------------------------------
// NewMiningJob (0x15) - Server to Client
// ----------------------------------------------------------------------------
struct NewMiningJob {
    uint32_t                channel_id{0};
    uint32_t                job_id{0};
    std::optional<uint32_t> min_ntime;
    uint32_t                version{0};
    std::array<uint8_t, 32> merkle_root{};

    void serialize(Writer& w) const {
        w.write_u32(channel_id);
        w.write_u32(job_id);
        w.write_option_u32(min_ntime);
        w.write_u32(version);
        // Per the SV2 spec, NewMiningJob.merkle_root is B0_32 (a length-prefixed
        // byte array), NOT a bare U256. Real SV2 miners (e.g. ESP-Miner/Bitaxe)
        // parse the leading length byte, so a fixed 32-byte encoding desyncs them.
        w.write_b0_32(std::vector<uint8_t>(merkle_root.begin(), merkle_root.end()));
    }
};

// ----------------------------------------------------------------------------
// SubmitSharesStandard (0x1a) - Client to Server
// ----------------------------------------------------------------------------
struct SubmitSharesStandard {
    uint32_t channel_id{0};
    uint32_t sequence_number{0};
    uint32_t job_id{0};
    uint32_t nonce{0};
    uint32_t ntime{0};
    uint32_t version{0};

    void deserialize(Reader& r) {
        channel_id      = r.read_u32();
        sequence_number = r.read_u32();
        job_id          = r.read_u32();
        nonce           = r.read_u32();
        ntime           = r.read_u32();
        version         = r.read_u32();
    }
};

// ----------------------------------------------------------------------------
// SubmitSharesExtended (0x1b) - Client to Server
// ----------------------------------------------------------------------------
struct SubmitSharesExtended {
    uint32_t             channel_id{0};
    uint32_t             sequence_number{0};
    uint32_t             job_id{0};
    uint32_t             nonce{0};
    uint32_t             ntime{0};
    uint32_t             version{0};
    std::vector<uint8_t> extranonce;

    void deserialize(Reader& r) {
        channel_id      = r.read_u32();
        sequence_number = r.read_u32();
        job_id          = r.read_u32();
        nonce           = r.read_u32();
        ntime           = r.read_u32();
        version         = r.read_u32();
        extranonce      = r.read_b0_32();
    }
};

// ----------------------------------------------------------------------------
// SubmitShares.Success (0x1c) - Server to Client
// ----------------------------------------------------------------------------
struct SubmitSharesSuccess {
    uint32_t channel_id{0};
    uint32_t last_sequence_number{0};
    uint32_t new_submits_accepted_count{0};
    uint64_t new_shares_sum{0};

    void serialize(Writer& w) const {
        w.write_u32(channel_id);
        w.write_u32(last_sequence_number);
        w.write_u32(new_submits_accepted_count);
        w.write_u64(new_shares_sum);
    }
};

// ----------------------------------------------------------------------------
// SubmitShares.Error (0x1d) - Server to Client
// ----------------------------------------------------------------------------
struct SubmitSharesError {
    uint32_t    channel_id{0};
    uint32_t    sequence_number{0};
    std::string error_code;

    void serialize(Writer& w) const {
        w.write_u32(channel_id);
        w.write_u32(sequence_number);
        w.write_str0_255(error_code);
    }
};

// ----------------------------------------------------------------------------
// SetTarget (0x21) - Server to Client
// ----------------------------------------------------------------------------
struct SetTarget {
    uint32_t                channel_id{0};
    std::array<uint8_t, 32> maximum_target{};

    void serialize(Writer& w) const {
        w.write_u32(channel_id);
        w.write_u256(maximum_target);
    }
};

// ----------------------------------------------------------------------------
// SetNewPrevHash (0x20) - Server to Client
// ----------------------------------------------------------------------------
struct SetNewPrevHash {
    uint32_t                channel_id{0};
    uint32_t                job_id{0};
    std::array<uint8_t, 32> prev_hash{};
    uint32_t                min_ntime{0};
    uint32_t                nbits{0};

    void serialize(Writer& w) const {
        w.write_u32(channel_id);
        w.write_u32(job_id);
        w.write_u256(prev_hash);
        w.write_u32(min_ntime);
        w.write_u32(nbits);
    }
};

// ----------------------------------------------------------------------------
// SetCustomMiningJob (0x22) - Client to Server
// ----------------------------------------------------------------------------
struct SetCustomMiningJob {
    uint32_t                             channel_id{0};
    uint32_t                             request_id{0};
    std::vector<uint8_t>                 mining_job_token;
    uint32_t                             version{0};
    std::array<uint8_t, 32>              prev_hash{};
    uint32_t                             min_ntime{0};
    uint32_t                             nbits{0};
    uint32_t                             coinbase_tx_version{1};
    std::vector<uint8_t>                 coinbase_prefix;
    uint32_t                             coinbase_tx_input_nSequence{0xFFFFFFFFu};
    std::vector<uint8_t>                 coinbase_tx_outputs;
    uint32_t                             coinbase_tx_locktime{0};
    std::vector<std::array<uint8_t, 32>> merkle_path;

    void deserialize(Reader& r) {
        channel_id                  = r.read_u32();
        request_id                  = r.read_u32();
        mining_job_token            = r.read_b0_255();
        version                     = r.read_u32();
        prev_hash                   = r.read_u256();
        min_ntime                   = r.read_u32();
        nbits                       = r.read_u32();
        coinbase_tx_version         = r.read_u32();
        coinbase_prefix             = r.read_b0_255();
        coinbase_tx_input_nSequence = r.read_u32();
        coinbase_tx_outputs         = r.read_b0_64k();
        coinbase_tx_locktime        = r.read_u32();
        merkle_path                 = r.read_seq0_255_u256();
    }
};

// ----------------------------------------------------------------------------
// SetCustomMiningJob.Success (0x23) - Server to Client
// ----------------------------------------------------------------------------
struct SetCustomMiningJobSuccess {
    uint32_t channel_id{0};
    uint32_t request_id{0};
    uint32_t job_id{0};

    void serialize(Writer& w) const {
        w.write_u32(channel_id);
        w.write_u32(request_id);
        w.write_u32(job_id);
    }
};

// ----------------------------------------------------------------------------
// SetCustomMiningJob.Error (0x24) - Server to Client
// ----------------------------------------------------------------------------
struct SetCustomMiningJobError {
    uint32_t    channel_id{0};
    uint32_t    request_id{0};
    std::string error_code;

    void serialize(Writer& w) const {
        w.write_u32(channel_id);
        w.write_u32(request_id);
        w.write_str0_255(error_code);
    }
};

// Helper: Wrap a serialized payload with its header
inline std::vector<uint8_t> wrap_message(uint16_t ext_type, uint8_t msg_type, const Writer& payload) {
    Writer w;
    Header h;
    // Set the channel_msg bit (MSB of extension_type) for channel-scoped messages, per spec.
    h.extension_type = is_channel_msg(msg_type)
                           ? static_cast<uint16_t>(ext_type | CHANNEL_MSG_BIT)
                           : ext_type;
    h.msg_type       = msg_type;
    h.msg_length     = static_cast<uint32_t>(payload.data().size());
    h.serialize(w);
    w.data().insert(w.data().end(), payload.data().begin(), payload.data().end());
    return std::move(w.data());
}

} // namespace mkpool::sv2
