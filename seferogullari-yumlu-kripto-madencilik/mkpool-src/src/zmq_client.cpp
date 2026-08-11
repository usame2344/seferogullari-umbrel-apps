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
// File:        zmq_client.cpp
// Description: ZeroMQ subscriber for bitcoind block-hash notifications.
// Created:     2025-11-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "zmq_client.hpp"
#include "logger.hpp"

namespace mkpool {

	ZMQClient::ZMQClient(boost::asio::io_context& io_context,
		const std::string& zmqAddress)
		: io_context_(io_context),
		zmqAddress_(zmqAddress),
		running_(false)
	{
		try {
			zmqContext_ = std::make_unique<zmq::context_t>(1);
			zmqSocket_ = std::make_unique<zmq::socket_t>(*zmqContext_, zmq::socket_type::sub);
			
			// Connect to Bitcoin node's ZMQ publisher
			zmqSocket_->connect(zmqAddress_);
			
			// Get the native file descriptor for integration with Boost.Asio
			int fd = zmqSocket_->get(zmq::sockopt::fd);
			
			// Create stream_descriptor for async I/O notifications
			descriptor_ = std::make_unique<boost::asio::posix::stream_descriptor>(io_context_, fd);
			
			mkpool::Logger::info("[ZMQClient] Connected to ZMQ endpoint: {}", zmqAddress_);
		}
		catch (const zmq::error_t& e) {
			mkpool::Logger::error("[ZMQClient] Failed to initialize ZMQ: {}", e.what());
			throw;
		}
		catch (const std::exception& e) {
			mkpool::Logger::error("[ZMQClient] Failed to initialize ZMQ descriptor: {}", e.what());
			throw;
		}
	}

	ZMQClient::~ZMQClient() {
		stop();
	}

	void ZMQClient::subscribe(const std::string& topic) {
		try {
			zmqSocket_->set(zmq::sockopt::subscribe, topic);
			mkpool::Logger::info("[ZMQClient] Subscribed to topic: {}", topic);
		}
		catch (const zmq::error_t& e) {
			mkpool::Logger::error("[ZMQClient] Failed to subscribe to {}: {}", topic, e.what());
		}
	}

	void ZMQClient::unsubscribe(const std::string& topic) {
		try {
			zmqSocket_->set(zmq::sockopt::unsubscribe, topic);
			mkpool::Logger::info("[ZMQClient] Unsubscribed from topic: {}", topic);
		}
		catch (const zmq::error_t& e) {
			mkpool::Logger::error("[ZMQClient] Failed to unsubscribe from {}: {}", topic, e.what());
		}
	}

	void ZMQClient::start(ZMQCallback callback) {
		callback_ = std::move(callback);
		running_ = true;
		mkpool::Logger::info("[ZMQClient] Starting message reception");
		asyncReceive();
	}

	void ZMQClient::stop() {
		// Idempotent stop
		if (stopped_) 
			return;

		stopped_ = true;
		running_ = false;

		try {
			if (descriptor_) {
				boost::system::error_code ec;
				descriptor_->cancel(ec);
				// CRITICAL: descriptor_ was constructed from the ZMQ socket's
				// internal fd (ZMQ_FD). Boost.Asio's posix::stream_descriptor owns
				// its fd and ::close()s it on destruction. Destroying it here would
				// close libzmq's signaler fd out from under ZMQ, so the subsequent
				// socket/context teardown hits "Bad file descriptor" in
				// signaler.cpp and abort()s -> core dump on every SIGTERM. Release
				// the fd so Asio relinquishes ownership WITHOUT closing it; libzmq
				// then closes it correctly via zmqSocket_->close() below.
				descriptor_->release();
				descriptor_.reset();
			}
			if (zmqSocket_) {
				zmqSocket_->close();
			}
			mkpool::Logger::info("[ZMQClient] Stopped");
		}
		catch (const std::exception& e) {
			mkpool::Logger::error("[ZMQClient] Exception during stop: {}", e.what());
		}
	}

	bool ZMQClient::isConnected() const {
		return zmqSocket_ && running_;
	}

	void ZMQClient::asyncReceive() {
		if (!running_ || !descriptor_) return;

		// Wait for the ZMQ socket to become readable
		descriptor_->async_wait(boost::asio::posix::stream_descriptor::wait_read,
			[this](const boost::system::error_code& ec) {
				if (ec) {
					if (ec != boost::asio::error::operation_aborted) {
						mkpool::Logger::error("[ZMQClient] Wait error: {}", ec.message());
					}
					return;
				}

				if (!running_) return;

				try {
					// EDGE-TRIGGERED ZMQ fd: async_wait only fires on the transition
					// into "events pending". If messages remain queued when we re-arm,
					// the fd will NOT fire again (POLLIN stays set, so there is no new
					// edge) and the loop goes permanently silent after the first
					// message. Drain EVERY queued message now, until ZMQ_POLLIN clears,
					// before re-arming async_wait.
					while (running_) {
						int events = zmqSocket_->get(zmq::sockopt::events);
						if (!(events & ZMQ_POLLIN)) break;

						zmq::message_t topicMsg;
						zmq::message_t dataMsg;
						zmq::message_t seqMsg;

						// Receive multipart message with non-blocking flag
						auto result = zmqSocket_->recv(topicMsg, zmq::recv_flags::dontwait);
						if (!result || *result == 0) break;  // EAGAIN: socket fully drained

						// Consume the remaining frames of this multipart message
						auto dataResult = zmqSocket_->recv(dataMsg, zmq::recv_flags::dontwait);
						auto seqResult = zmqSocket_->recv(seqMsg, zmq::recv_flags::dontwait);
						(void)dataResult; (void)seqResult;

						std::string topic(static_cast<char*>(topicMsg.data()), topicMsg.size());
						std::string data(static_cast<char*>(dataMsg.data()), dataMsg.size());

						mkpool::Logger::info("[ZMQClient] Received ZMQ notification: {}", topic);

						if (callback_) {
							callback_(topic, data);
						}
					}
				}
				catch (const zmq::error_t& e) {
					if (e.num() != EAGAIN && e.num() != EINTR) {
						mkpool::Logger::error("[ZMQClient] Error receiving message: {}", e.what());
					}
				}

				// Continue listening for next message
				if (running_) {
					asyncReceive();
				}
			});
	}

} // namespace mkpool