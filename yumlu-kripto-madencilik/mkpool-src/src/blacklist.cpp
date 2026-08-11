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
// File:        blacklist.cpp
// Description: In-memory address/worker blacklist + free-hashrate grant list.
// Created:     2026-06-10
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "blacklist.hpp"
#include "database.hpp"

#include <cctype>

namespace mkpool {

Blacklist& Blacklist::instance() {
    static Blacklist inst;
    return inst;
}

std::string Blacklist::normalize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) out.push_back(static_cast<char>(std::tolower(uc)));
    }
    return out;
}

void Blacklist::reload() {
    std::unordered_set<std::string> addrs;
    std::vector<std::string>        words;
    std::unordered_set<std::string> grants;

    try {
        for (const auto& r : Database::query("SELECT address FROM address_blacklist"))
            if (!r.empty() && !r[0].empty()) addrs.insert(r[0]);

        for (const auto& r : Database::query("SELECT word FROM workers_blacklist")) {
            if (r.empty()) continue;
            std::string n = normalize(r[0]);
            if (!n.empty()) words.push_back(std::move(n));
        }

        for (const auto& r : Database::query(
                 "SELECT address FROM free_hashrate_grants "
                 "WHERE expires_at IS NULL OR expires_at > now()"))
            if (!r.empty() && !r[0].empty()) grants.insert(r[0]);
    } catch (const std::exception& e) {
        spdlog::error("[Blacklist] reload failed, keeping previous state: {}", e.what());
        return;
    }

    std::size_t na, nw, ng;
    {
        std::lock_guard lk(mtx_);
        addresses_.swap(addrs);
        banned_words_.swap(words);
        free_grants_.swap(grants);
        na = addresses_.size();
        nw = banned_words_.size();
        ng = free_grants_.size();
    }
    spdlog::info("[Blacklist] loaded {} blocked addresses, {} worker words, {} free-hashrate grants",
                 na, nw, ng);
}

bool Blacklist::address_blocked(const std::string& addr) const {
    std::lock_guard lk(mtx_);
    return addresses_.find(addr) != addresses_.end();
}

bool Blacklist::worker_blocked(const std::string& worker) const {
    const std::string n = normalize(worker);
    if (n.empty()) return false;
    std::lock_guard lk(mtx_);
    for (const auto& w : banned_words_)
        if (n.find(w) != std::string::npos) return true;
    return false;
}

bool Blacklist::worker_is_free(const std::string& worker) const {
    const std::string n = normalize(worker);
    return n.find(kFreeToken) != std::string::npos;
}

bool Blacklist::free_granted(const std::string& addr) const {
    std::lock_guard lk(mtx_);
    return free_grants_.find(addr) != free_grants_.end();
}

} // namespace mkpool
