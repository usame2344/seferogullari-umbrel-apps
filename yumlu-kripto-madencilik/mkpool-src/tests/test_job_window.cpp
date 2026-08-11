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
// File:        test_job_window.cpp
// Description: Unit tests for the rolling job window.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include <catch2/catch_test_macros.hpp>
#include "job_window.hpp"

#include <string>

using mkpool::JobWindow;

TEST_CASE("push/find/latest roundtrip", "[jobwindow]") {
    JobWindow<std::string> w(4);
    w.push("a", "vA");
    w.push("b", "vB");
    w.push("c", "vC");
    CHECK(w.size() == 3);
    REQUIRE(w.find("a"));
    CHECK(*w.find("a") == "vA");
    CHECK(*w.latest() == "vC");
}

TEST_CASE("oldest entries evicted at capacity", "[jobwindow]") {
    JobWindow<int> w(2);
    w.push("a", 1);
    w.push("b", 2);
    w.push("c", 3);
    CHECK(w.size() == 2);
    CHECK_FALSE(w.find("a").has_value());
    REQUIRE(w.find("b"));
    REQUIRE(w.find("c"));
    CHECK(*w.find("b") == 2);
    CHECK(*w.find("c") == 3);
}

TEST_CASE("clear empties window", "[jobwindow]") {
    JobWindow<int> w(8);
    for (int i = 0; i < 5; ++i) w.push(std::to_string(i), i);
    w.clear();
    CHECK(w.size() == 0);
    CHECK_FALSE(w.latest().has_value());
}
