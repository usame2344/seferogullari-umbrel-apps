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
// File:        blacklist.hpp
// Description: In-memory address/worker blacklist + free-hashrate grant list.
// Created:     2026-06-10
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace mkpool {

// Authorize-time blacklist. The source of truth is three DB tables
// (address_blacklist, workers_blacklist, free_hashrate_grants); this class
// holds an in-memory copy refreshed periodically so the stratum hot path never
// touches the database (important under NiceHash/Braiins reconnect churn).
//
// Matching rules (deliberately simple, no regex):
//   - addresses : exact, case-sensitive set lookup.
//   - workers   : normalized substring (lowercase + [a-z0-9] only), so one row
//                 "fuck" also blocks "Fuck123" and "f.u.c.k".
//   - free grant: exact address lookup; only granted addresses may use the
//                 reserved "FREE-HASHRATE" worker name.
class Blacklist {
public:
    static Blacklist& instance();

    // (Re)load all three tables from the DB and atomically swap them in.
    // On any DB error the previous in-memory state is kept.
    void reload();

    // lowercase + keep only [a-z0-9] (drops '.', '-', '_', spaces, etc.).
    static std::string normalize(std::string_view s);

    bool address_blocked(const std::string& addr) const;   // exact
    bool worker_blocked(const std::string& worker) const;   // normalized substring
    bool worker_is_free(const std::string& worker) const;   // contains the reserved token
    bool free_granted(const std::string& addr) const;       // exact

private:
    Blacklist() = default;

    // Normalized reserved worker token: "FREE-HASHRATE" -> "freehashrate".
    static constexpr std::string_view kFreeToken = "freehashrate";

    mutable std::mutex mtx_;
    std::unordered_set<std::string> addresses_;
    std::vector<std::string>        banned_words_;  // pre-normalized
    std::unordered_set<std::string> free_grants_;
};

} // namespace mkpool
