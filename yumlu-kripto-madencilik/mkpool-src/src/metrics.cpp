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
// File:        metrics.cpp
// Description: Prometheus metrics exposition.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "metrics.hpp"

#if MKPOOL_HAS_METRICS
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#endif

namespace mkpool::metrics {
#if MKPOOL_HAS_METRICS

namespace {
struct State {
    std::unique_ptr<prometheus::Exposer> exposer;
    std::shared_ptr<prometheus::Registry> registry;

    prometheus::Family<prometheus::Counter>*   share_accepted_family{};
    prometheus::Counter*                         share_accepted{};
    prometheus::Family<prometheus::Counter>*   share_rejected_family{};
    prometheus::Counter*                         share_stale{};
    prometheus::Counter*                         share_invalid{};
    prometheus::Counter*                         block_found{};
    prometheus::Family<prometheus::Counter>*   conn_accepted_family{};
    prometheus::Counter*                         conn_accepted{};
    prometheus::Family<prometheus::Counter>*   conn_rejected_family{};

    prometheus::Family<prometheus::Gauge>*     conn_open_family{};
    prometheus::Gauge*                            conn_open{};
    prometheus::Gauge*                            active_miners{};
    prometheus::Gauge*                            db_queue_depth{};

    prometheus::Family<prometheus::Histogram>* share_us_family{};
    prometheus::Histogram*                       share_us{};
    prometheus::Histogram*                       template_us{};
};

State* g_state = nullptr;

prometheus::Histogram::BucketBoundaries default_buckets() {
    return { 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000, 250000 };
}
} // anon

void init(const std::string& bind, std::uint16_t port) {
    if (g_state) return;
    g_state = new State{};
    g_state->registry = std::make_shared<prometheus::Registry>();
    g_state->exposer  = std::make_unique<prometheus::Exposer>(fmt::format("{}:{}", bind, port));
    g_state->exposer->RegisterCollectable(g_state->registry);

    auto& reg = *g_state->registry;
    auto& sa = prometheus::BuildCounter().Name("mkpool_shares_accepted_total").Help("Accepted shares").Register(reg);
    g_state->share_accepted_family = &sa;
    g_state->share_accepted = &sa.Add({});

    auto& sr = prometheus::BuildCounter().Name("mkpool_shares_rejected_total").Help("Rejected shares").Register(reg);
    g_state->share_rejected_family = &sr;
    g_state->share_stale = &sr.Add({{"reason","stale"}});
    g_state->share_invalid = &sr.Add({{"reason","invalid"}});

    auto& bf = prometheus::BuildCounter().Name("mkpool_blocks_found_total").Help("Blocks found").Register(reg);
    g_state->block_found = &bf.Add({});

    auto& ca = prometheus::BuildCounter().Name("mkpool_connections_accepted_total").Help("Accepted connections").Register(reg);
    g_state->conn_accepted_family = &ca;
    g_state->conn_accepted = &ca.Add({});

    auto& cr = prometheus::BuildCounter().Name("mkpool_connections_rejected_total").Help("Rejected connections").Register(reg);
    g_state->conn_rejected_family = &cr;

    auto& co = prometheus::BuildGauge().Name("mkpool_connections_open").Help("Open connections").Register(reg);
    g_state->conn_open_family = &co;
    g_state->conn_open = &co.Add({});

    auto& am = prometheus::BuildGauge().Name("mkpool_active_miners").Help("Active miners").Register(reg);
    g_state->active_miners = &am.Add({});

    auto& dq = prometheus::BuildGauge().Name("mkpool_db_queue_depth").Help("DB worker queue depth").Register(reg);
    g_state->db_queue_depth = &dq.Add({});

    auto& sh = prometheus::BuildHistogram().Name("mkpool_share_processing_us").Help("Share processing microseconds").Register(reg);
    g_state->share_us_family = &sh;
    g_state->share_us = &sh.Add({}, default_buckets());

    auto& th = prometheus::BuildHistogram().Name("mkpool_template_build_us").Help("Template build microseconds").Register(reg);
    g_state->template_us = &th.Add({}, default_buckets());
}

void shutdown() {
    delete g_state;
    g_state = nullptr;
}

void inc_share_accepted()                       { if (g_state) g_state->share_accepted->Increment(); }
void inc_share_rejected(const char* reason)     {
    if (!g_state) return;
    g_state->share_rejected_family->Add({{"reason", reason}}).Increment();
}
void inc_share_stale()                          { if (g_state) g_state->share_stale->Increment(); }
void inc_share_invalid()                        { if (g_state) g_state->share_invalid->Increment(); }
void inc_block_found()                          { if (g_state) g_state->block_found->Increment(); }
void inc_connections_accepted()                 { if (g_state) g_state->conn_accepted->Increment(); }
void inc_connections_rejected(const char* r)    {
    if (!g_state) return;
    g_state->conn_rejected_family->Add({{"reason", r}}).Increment();
}
void set_connections_open(std::int64_t v)       { if (g_state) g_state->conn_open->Set((double)v); }
void set_active_miners(std::int64_t v)          { if (g_state) g_state->active_miners->Set((double)v); }
void set_db_queue_depth(std::int64_t v)         { if (g_state) g_state->db_queue_depth->Set((double)v); }
void observe_share_processing_us(double v)      { if (g_state) g_state->share_us->Observe(v); }
void observe_template_build_us(double v)        { if (g_state) g_state->template_us->Observe(v); }

#else // !MKPOOL_HAS_METRICS

void init(const std::string&, std::uint16_t) {}
void shutdown() {}
void inc_share_accepted() {}
void inc_share_rejected(const char*) {}
void inc_share_stale() {}
void inc_share_invalid() {}
void inc_block_found() {}
void inc_connections_accepted() {}
void inc_connections_rejected(const char*) {}
void set_connections_open(std::int64_t) {}
void set_active_miners(std::int64_t) {}
void set_db_queue_depth(std::int64_t) {}
void observe_share_processing_us(double) {}
void observe_template_build_us(double) {}

#endif
} // namespace mkpool::metrics
