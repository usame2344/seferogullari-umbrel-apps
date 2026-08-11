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
// File:        control_server.cpp
// Description: Unix-domain runtime control/admin socket. Line in, JSON out.
// Created:     2026-07-18
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "control_server.hpp"

#include <spdlog/spdlog.h>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <filesystem>
#include <istream>
#include <sys/stat.h>
#include <unistd.h>

namespace mkpool {

using local = boost::asio::local::stream_protocol;

namespace {

// One command exchange: read a single line, dispatch, write the reply, close.
class ControlConn : public std::enable_shared_from_this<ControlConn> {
public:
    ControlConn(local::socket sock, ControlServer::Handler handler)
        : sock_(std::move(sock)), handler_(std::move(handler)) {}

    void start() {
        auto self = shared_from_this();
        boost::asio::async_read_until(sock_, buf_, '\n',
            [self](const boost::system::error_code& ec, std::size_t) { self->on_read(ec); });
    }

private:
    void on_read(const boost::system::error_code& ec) {
        // A client that sends a line then closes yields eof alongside data.
        if (ec && ec != boost::asio::error::eof) return;
        std::string line;
        {
            std::istream is(&buf_);
            std::getline(is, line);
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::string resp;
        try {
            resp = handler_(line);
        } catch (const std::exception& e) {
            resp = std::string("{\"error\":\"handler exception: ") + e.what() + "\"}";
        }
        if (resp.empty() || resp.back() != '\n') resp.push_back('\n');

        auto self = shared_from_this();
        auto out = std::make_shared<std::string>(std::move(resp));
        boost::asio::async_write(sock_, boost::asio::buffer(*out),
            [self, out](const boost::system::error_code&, std::size_t) {
                boost::system::error_code ig;
                self->sock_.shutdown(local::socket::shutdown_both, ig);
            });
    }

    local::socket sock_;
    ControlServer::Handler handler_;
    boost::asio::streambuf buf_;
};

} // namespace

ControlServer::ControlServer(boost::asio::io_context& io, std::string path, Handler handler)
    : io_(io), path_(std::move(path)), handler_(std::move(handler)), acceptor_(io) {}

void ControlServer::start() {
    if (path_.empty()) {
        spdlog::info("[Control] disabled (no socket path configured)");
        return;
    }
    try {
        std::error_code fec;
        const auto parent = std::filesystem::path(path_).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, fec);
        ::unlink(path_.c_str());  // clear any stale socket from a prior run

        acceptor_.open();
        acceptor_.bind(local::endpoint(path_));
        acceptor_.listen();
        ::chmod(path_.c_str(), 0600);  // owner-only

        running_ = true;
        spdlog::info("[Control] listening on {}", path_);
        do_accept();
    } catch (const std::exception& e) {
        // Never let a control-socket failure take down the pool.
        spdlog::error("[Control] failed to start on '{}': {} (control disabled)", path_, e.what());
        running_ = false;
    }
}

void ControlServer::stop() {
    if (!running_) return;
    running_ = false;
    boost::system::error_code ig;
    acceptor_.close(ig);
    ::unlink(path_.c_str());
}

void ControlServer::do_accept() {
    acceptor_.async_accept([this](const boost::system::error_code& ec, local::socket sock) {
        if (!running_) return;
        if (!ec) {
            std::make_shared<ControlConn>(std::move(sock), handler_)->start();
        }
        do_accept();
    });
}

} // namespace mkpool
