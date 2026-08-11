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
// File:        beast_client.hpp
// Description: Boost.Beast HTTP JSON-RPC client interface.
// Created:     2025-04-06
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <functional>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace mkpool
{
	class BeastClient : public std::enable_shared_from_this<BeastClient>
	{
	public:
		using json = nlohmann::json;
		using Callback = std::function<void(const boost::system::error_code&, const json&)>;

		// Constructor: Initialize with io_context, host, port, and optional auth header
		BeastClient(boost::asio::io_context& ioc,
			const std::string& host,
			const std::string& port,
			const std::string& authHeader = "");

		// Start the async HTTP POST request with the given JSON request and callback
		void run(const json& request, Callback callback);

	private:
		// Async operation handlers
		void on_resolve(boost::system::error_code ec, boost::asio::ip::tcp::resolver::results_type results);
		void on_connect(boost::system::error_code ec, boost::asio::ip::tcp::resolver::results_type::endpoint_type);
		void on_write(boost::system::error_code ec, std::size_t bytes_transferred);
		void on_read(boost::system::error_code ec, std::size_t bytes_transferred);

		// Members
		boost::asio::io_context& ioc_;                          // Reference to the io_context
		boost::asio::ip::tcp::resolver resolver_;               // Resolver for DNS lookup
		boost::beast::tcp_stream stream_;                       // TCP stream for the connection
		boost::beast::flat_buffer buffer_;                      // Buffer for reading responses
		boost::beast::http::request<boost::beast::http::string_body> req_;  // HTTP request object
		boost::beast::http::response<boost::beast::http::string_body> res_; // HTTP response object
		std::string host_;                                      // Target host
		std::string port_;                                      // Target port
		std::string authHeader_;                                // Optional authentication header
		Callback callback_;                                     // Callback to invoke with result
		json request_;                                          // JSON request payload
	};

} // namespace mkpool