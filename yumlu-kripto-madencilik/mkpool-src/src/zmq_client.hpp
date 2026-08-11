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
// File:        zmq_client.hpp
// Description: ZeroMQ block-notification subscriber interface.
// Created:     2025-11-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

namespace mkpool {

	using json = nlohmann::json;

	class ZMQClient : public std::enable_shared_from_this<ZMQClient> {
	public:
		using ZMQCallback = std::function<void(const std::string& topic, const std::string& data)>;

		ZMQClient(boost::asio::io_context& io_context,
			const std::string& zmqAddress);

		~ZMQClient();

		// Subscribe to ZMQ topics
		void subscribe(const std::string& topic);
		void unsubscribe(const std::string& topic);

		// Start listening for ZMQ messages
		void start(ZMQCallback callback);
		void stop();

		// Check if ZMQ is connected
		bool isConnected() const;

	private:
		void asyncReceive();

		boost::asio::io_context& io_context_;
		std::unique_ptr<zmq::context_t> zmqContext_;
		std::unique_ptr<zmq::socket_t> zmqSocket_;
		std::string zmqAddress_;
		ZMQCallback callback_;
		std::unique_ptr<boost::asio::posix::stream_descriptor> descriptor_;
		bool running_{ false };
		bool stopped_{ false }; // prevent double stop
	};

} // namespace mkpool
