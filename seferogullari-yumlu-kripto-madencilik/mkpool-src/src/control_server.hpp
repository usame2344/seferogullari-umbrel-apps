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
// File:        control_server.hpp
// Description: Unix-domain runtime control/admin socket. Line in, JSON out.
// Created:     2026-07-18
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <functional>
#include <memory>
#include <string>

namespace mkpool {

// A tiny line-based Unix-domain control socket. Each accepted connection sends
// exactly one command line ("stats", "reconnect 1.2.3.4 3333 5", ...) and gets
// back one response line (JSON), after which the connection is closed. All
// mining/business logic lives in the supplied handler (PoolManager); this class
// is pure transport so it stays isolated from the hot path.
class ControlServer {
public:
    using Handler = std::function<std::string(const std::string&)>;

    ControlServer(boost::asio::io_context& io, std::string path, Handler handler);

    // Bind + listen + begin accepting. No-op (logged) if the path is empty or
    // binding fails - a broken control socket must never stop the pool.
    void start();
    void stop();

    [[nodiscard]] const std::string& path() const { return path_; }

private:
    void do_accept();

    boost::asio::io_context& io_;
    std::string path_;
    Handler handler_;
    boost::asio::local::stream_protocol::acceptor acceptor_;
    bool running_{false};
};

} // namespace mkpool
