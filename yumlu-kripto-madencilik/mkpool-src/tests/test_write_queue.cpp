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
// File:        test_write_queue.cpp
// Description: Unit tests for the per-session write queue.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include <catch2/catch_test_macros.hpp>
#include "write_queue.hpp"

#include <boost/asio.hpp>
#include <string>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using boost::asio::ip::tcp;

TEST_CASE("WriteQueue serializes writes over a connected pair", "[writequeue]") {
    asio::io_context io;
    tcp::acceptor acc(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acc.local_endpoint().port();

    tcp::socket reader(io);
    tcp::socket writer(io);

    acc.async_accept(reader, [&](const boost::system::error_code&){});
    writer.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                         [&](const boost::system::error_code&){});
    io.run_for(std::chrono::milliseconds(200));
    io.restart();

    mkpool::WriteQueue<tcp::socket> wq(writer);
    wq.push("hello\n");
    wq.push("world\n");

    std::string buf;
    buf.resize(64);
    std::size_t total = 0;
    asio::async_read(reader, asio::buffer(buf.data() + total, 12),
                     [&](const boost::system::error_code&, std::size_t n) { total = n; });
    io.run_for(std::chrono::milliseconds(500));
    REQUIRE(total >= 12);
    CHECK(buf.substr(0, 12) == "hello\nworld\n");
}
