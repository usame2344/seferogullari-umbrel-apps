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
// File:        sv2_codec.hpp
// Description: Stratum V2 frame encoding and decoding.
// Created:     2026-05-27
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <array>
#include <optional>
#include <cstring>
#include <stdexcept>

namespace mkpool::sv2 {

// ----------------------------------------------------------------------------
// SV2 Writer / Serializer
// ----------------------------------------------------------------------------
class Writer {
public:
    Writer() = default;

    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t>& data() { return buf_; }

    void write_u8(uint8_t val) {
        buf_.push_back(val);
    }

    void write_bool(bool val) {
        buf_.push_back(val ? 1 : 0);
    }

    void write_u16(uint16_t val) {
        buf_.push_back(static_cast<uint8_t>(val & 0xFF));
        buf_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    }

    void write_u24(uint32_t val) {
        buf_.push_back(static_cast<uint8_t>(val & 0xFF));
        buf_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    }

    void write_u32(uint32_t val) {
        buf_.push_back(static_cast<uint8_t>(val & 0xFF));
        buf_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    }

    void write_u64(uint64_t val) {
        for (int i = 0; i < 8; ++i) {
            buf_.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }
    }

    void write_u256(const std::array<uint8_t, 32>& val) {
        buf_.insert(buf_.end(), val.begin(), val.end());
    }

    void write_f32(float val) {
        uint32_t tmp;
        std::memcpy(&tmp, &val, sizeof(float));
        write_u32(tmp);
    }

    void write_str0_255(std::string_view val) {
        size_t len = std::min(val.size(), static_cast<size_t>(255));
        write_u8(static_cast<uint8_t>(len));
        buf_.insert(buf_.end(), val.begin(), val.begin() + len);
    }

    void write_b0_32(const std::vector<uint8_t>& val) {
        size_t len = std::min(val.size(), static_cast<size_t>(32));
        write_u8(static_cast<uint8_t>(len));
        buf_.insert(buf_.end(), val.begin(), val.begin() + len);
    }

    void write_b0_255(const std::vector<uint8_t>& val) {
        size_t len = std::min(val.size(), static_cast<size_t>(255));
        write_u8(static_cast<uint8_t>(len));
        buf_.insert(buf_.end(), val.begin(), val.begin() + len);
    }

    void write_b0_64k(const std::vector<uint8_t>& val) {
        size_t len = std::min(val.size(), static_cast<size_t>(65535));
        write_u16(static_cast<uint16_t>(len));
        buf_.insert(buf_.end(), val.begin(), val.begin() + len);
    }

    void write_option_u32(std::optional<uint32_t> val) {
        if (val.has_value()) {
            write_u8(1);
            write_u32(*val);
        } else {
            write_u8(0);
        }
    }

    void write_seq0_255_u256(const std::vector<std::array<uint8_t, 32>>& val) {
        size_t len = std::min(val.size(), static_cast<size_t>(255));
        write_u8(static_cast<uint8_t>(len));
        for (size_t i = 0; i < len; ++i) {
            write_u256(val[i]);
        }
    }

private:
    std::vector<uint8_t> buf_;
};

// ----------------------------------------------------------------------------
// SV2 Reader / Deserializer
// ----------------------------------------------------------------------------
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size), pos_(0) {}
    Reader(const std::vector<uint8_t>& vec) : data_(vec.data()), size_(vec.size()), pos_(0) {}

    size_t remaining() const { return size_ - pos_; }
    size_t pos() const { return pos_; }

    uint8_t read_u8() {
        if (pos_ + 1 > size_) throw std::out_of_range("SV2 parse U8 out of range");
        return data_[pos_++];
    }

    bool read_bool() {
        return read_u8() != 0;
    }

    uint16_t read_u16() {
        if (pos_ + 2 > size_) throw std::out_of_range("SV2 parse U16 out of range");
        uint16_t val = data_[pos_] | (data_[pos_ + 1] << 8);
        pos_ += 2;
        return val;
    }

    uint32_t read_u24() {
        if (pos_ + 3 > size_) throw std::out_of_range("SV2 parse U24 out of range");
        uint32_t val = data_[pos_] | (data_[pos_ + 1] << 8) | (data_[pos_ + 2] << 16);
        pos_ += 3;
        return val;
    }

    uint32_t read_u32() {
        if (pos_ + 4 > size_) throw std::out_of_range("SV2 parse U32 out of range");
        uint32_t val = data_[pos_] | (data_[pos_ + 1] << 8) | (data_[pos_ + 2] << 16) | (data_[pos_ + 3] << 24);
        pos_ += 4;
        return val;
    }

    uint64_t read_u64() {
        if (pos_ + 8 > size_) throw std::out_of_range("SV2 parse U64 out of range");
        uint64_t val = 0;
        for (int i = 0; i < 8; ++i) {
            val |= (static_cast<uint64_t>(data_[pos_ + i]) << (i * 8));
        }
        pos_ += 8;
        return val;
    }

    std::array<uint8_t, 32> read_u256() {
        if (pos_ + 32 > size_) throw std::out_of_range("SV2 parse U256 out of range");
        std::array<uint8_t, 32> val;
        std::memcpy(val.data(), &data_[pos_], 32);
        pos_ += 32;
        return val;
    }

    float read_f32() {
        uint32_t tmp = read_u32();
        float val;
        std::memcpy(&val, &tmp, sizeof(float));
        return val;
    }

    std::string read_str0_255() {
        uint8_t len = read_u8();
        if (pos_ + len > size_) throw std::out_of_range("SV2 parse STR0_255 out of range");
        std::string val(reinterpret_cast<const char*>(&data_[pos_]), len);
        pos_ += len;
        return val;
    }

    std::vector<uint8_t> read_b0_32() {
        uint8_t len = read_u8();
        if (len > 32) throw std::runtime_error("B0_32 too long");
        if (pos_ + len > size_) throw std::out_of_range("SV2 parse B0_32 out of range");
        std::vector<uint8_t> val(&data_[pos_], &data_[pos_ + len]);
        pos_ += len;
        return val;
    }

    std::vector<uint8_t> read_b0_255() {
        uint8_t len = read_u8();
        if (pos_ + len > size_) throw std::out_of_range("SV2 parse B0_255 out of range");
        std::vector<uint8_t> val(&data_[pos_], &data_[pos_ + len]);
        pos_ += len;
        return val;
    }

    std::vector<uint8_t> read_b0_64k() {
        uint16_t len = read_u16();
        if (pos_ + len > size_) throw std::out_of_range("SV2 parse B0_64K out of range");
        std::vector<uint8_t> val(&data_[pos_], &data_[pos_ + len]);
        pos_ += len;
        return val;
    }

    std::optional<uint32_t> read_option_u32() {
        uint8_t active = read_u8();
        if (active) {
            return read_u32();
        }
        return std::nullopt;
    }

    std::vector<std::array<uint8_t, 32>> read_seq0_255_u256() {
        uint8_t len = read_u8();
        std::vector<std::array<uint8_t, 32>> val;
        val.reserve(len);
        for (uint8_t i = 0; i < len; ++i) {
            val.push_back(read_u256());
        }
        return val;
    }

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_;
};

} // namespace mkpool::sv2
