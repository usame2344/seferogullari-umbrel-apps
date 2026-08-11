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
// File:        beast_client.cpp
// Description: Boost.Beast HTTP/1.1 client for bitcoind JSON-RPC.
// Created:     2025-04-06
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "beast_client.hpp"
#include "logger.hpp"

namespace mkpool
{
	BeastClient::BeastClient(boost::asio::io_context& ioc,
		const std::string& host,
		const std::string& port,
		const std::string& authHeader)
		: ioc_(ioc),
		resolver_(boost::asio::make_strand(ioc)),  // Thread-safe resolver
		stream_(boost::asio::make_strand(ioc)),    // Thread-safe stream
		host_(host),
		port_(port),
		authHeader_(authHeader)
	{
	}

	void BeastClient::run(const json& request, Callback callback)
	{
		request_ = request;
		callback_ = callback;

		// Configure the HTTP POST request
		req_.version(11);  // HTTP 1.1
		req_.method(boost::beast::http::verb::post);
		req_.target("/");  // Adjust target if your endpoint differs
		req_.set(boost::beast::http::field::host, host_);
		req_.set(boost::beast::http::field::content_type, "application/json");
		req_.set(boost::beast::http::field::connection, "close");
		if (!authHeader_.empty()) {
			req_.set(boost::beast::http::field::authorization, "Basic " + authHeader_);
		}
		req_.body() = request_.dump();  // Serialize JSON to string
		req_.prepare_payload();         // Set Content-Length header

		// Start the async resolve operation
		resolver_.async_resolve(
			host_, port_,
			boost::beast::bind_front_handler(
				&BeastClient::on_resolve,
				shared_from_this()));
	}

	void BeastClient::on_resolve(boost::system::error_code ec,
		boost::asio::ip::tcp::resolver::results_type results)
	{
		if (ec) {
			mkpool::Logger::error("Resolve failed: {}", ec.message());
			callback_(ec, {});
			return;
		}

		// Set a timeout for the connection
		stream_.expires_after(std::chrono::seconds(10));
		stream_.async_connect(
			results,
			boost::beast::bind_front_handler(
				&BeastClient::on_connect,
				shared_from_this()));
	}

	void BeastClient::on_connect(boost::system::error_code ec,
		boost::asio::ip::tcp::resolver::results_type::endpoint_type)
	{
		if (ec) {
			mkpool::Logger::error("Connect failed: {}", ec.message());
			callback_(ec, {});
			return;
		}

		// Set a timeout for the write operation
		stream_.expires_after(std::chrono::seconds(10));
		boost::beast::http::async_write(
			stream_, req_,
			boost::beast::bind_front_handler(
				&BeastClient::on_write,
				shared_from_this()));
	}

	void BeastClient::on_write(boost::system::error_code ec, std::size_t /*bytes_transferred*/)
	{
		if (ec) {
			mkpool::Logger::error("Write failed: {}", ec.message());
			callback_(ec, {});
			return;
		}

		boost::beast::http::async_read(
			stream_, buffer_, res_,
			boost::beast::bind_front_handler(
				&BeastClient::on_read,
				shared_from_this()));
	}

	void BeastClient::on_read(boost::system::error_code ec, std::size_t /*bytes_transferred*/)
	{
		if (ec) {
			mkpool::Logger::error("Read failed: {}", ec.message());
			callback_(ec, {});
			return;
		}

		// Parse the response body as JSON
		try {
			mkpool::Logger::debug("Read debug: {}", res_.body());
			json response_json = json::parse(res_.body());
			callback_({}, response_json);  // Success: no error, parsed JSON
		}
		catch (const std::exception& e) {
			mkpool::Logger::error("JSON parse error: {}", e.what());
			callback_(boost::asio::error::invalid_argument, {});
		}

		// Clean up the connection
		boost::system::error_code shutdown_ec;
		stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, shutdown_ec);
	}
}