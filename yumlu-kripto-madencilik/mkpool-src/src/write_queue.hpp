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
// File:        write_queue.hpp
// Description: Per-session outbound write queue with a backpressure watermark.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <boost/asio.hpp>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <utility>

namespace mkpool {

    // A strand-bound, single-producer write queue. Caller MUST post `push` from
    // the owning strand. The queue uses chained async_write for ordering and
    // enforces a high-watermark (drop & disconnect when exceeded).
    template <class Stream>
    class WriteQueue {
    public:
        using DropHandler = std::function<void()>;

        explicit WriteQueue(Stream& s,
                            std::size_t high_water = 1u << 20 /* 1 MiB */)
            : stream_(s), high_water_(high_water) {}

        void set_drop_handler(DropHandler h) { drop_ = std::move(h); }

        // Bind the chained async_write completion handlers to the owning
        // session's strand. Without this the write chain re-issues async_write
        // from arbitrary io threads and can race with the strand-serialised
        // socket close in on_disconnect_local(), dereferencing a freed ASIO
        // descriptor_state (NULL) -> segfault under high connection churn.
        // The session MUST call this (from start()) before any push().
        void set_executor(boost::asio::any_io_executor ex) {
            ex_     = std::move(ex);
            has_ex_ = true;
        }

        // Stop the queue: no further async_write is issued. Call from the
        // owning strand when the session is being torn down.
        void stop() { stopped_ = true; }

        // The owning session MUST call this once (from start()) with a
        // weak_ptr to itself, so chained async_write handlers hold a
        // shared_ptr ref to the session for the duration of each write
        // and the queue is never accessed after its owner is destroyed.
        void set_owner(std::weak_ptr<void> owner) {
            owner_       = std::move(owner);
            has_owner_   = true;
        }

        // Returns false if the high watermark was exceeded; caller should
        // close the session.
        bool push(std::string msg) {
            if (stopped_) return false;
            queued_bytes_ += msg.size();
            if (queued_bytes_ > high_water_) {
                if (drop_) drop_();
                return false;
            }
            queue_.emplace_back(std::move(msg));
            if (!writing_) {
                writing_ = true;
                do_write();
            }
            return true;
        }

        [[nodiscard]] std::size_t queued() const noexcept { return queued_bytes_; }

    private:
        void do_write() {
            // Never issue I/O once the session is being torn down: the socket
            // may already be closed and its ASIO descriptor_state freed.
            if (stopped_) { writing_ = false; return; }
            // Reset the active buffer to the front item so the underlying
            // buffer pointer is stable for the duration of async_write.
            auto& front = queue_.front();
            // Keep the owning session alive for the duration of this write.
            // If an owner was installed and is already gone, stop the chain.
            std::shared_ptr<void> self;
            if (has_owner_) {
                self = owner_.lock();
                if (!self) {
                    writing_ = false;
                    return;
                }
            }
            auto handler =
                [this, self = std::move(self)]
                (const boost::system::error_code& ec, std::size_t /*n*/) {
                    if (ec) {
                        writing_ = false;
                        if (drop_) drop_();
                        return;
                    }
                    if (stopped_ || queue_.empty()) {
                        // Session torn down (or, defensively, empty deque):
                        // never deref an empty deque mid-flight.
                        writing_ = false;
                        return;
                    }
                    queued_bytes_ -= queue_.front().size();
                    queue_.pop_front();
                    if (queue_.empty()) {
                        writing_ = false;
                    } else {
                        do_write();
                    }
                };
            // Bind the completion to the owning strand so the chain stays
            // serialised with the session's close/read handlers (eliminates the
            // descriptor_state race). Fall back to an unbound handler only when
            // no executor was installed (e.g. standalone unit tests).
            if (has_ex_) {
                boost::asio::async_write(
                    stream_,
                    boost::asio::buffer(front.data(), front.size()),
                    boost::asio::bind_executor(ex_, std::move(handler)));
            } else {
                boost::asio::async_write(
                    stream_,
                    boost::asio::buffer(front.data(), front.size()),
                    std::move(handler));
            }
        }

        Stream& stream_;
        std::deque<std::string> queue_;
        std::size_t queued_bytes_{0};
        std::size_t high_water_;
        bool writing_{false};
        DropHandler drop_;
        std::weak_ptr<void> owner_;
        bool has_owner_{false};
        boost::asio::any_io_executor ex_{};
        bool has_ex_{false};
        bool stopped_{false};
    };

} // namespace mkpool
