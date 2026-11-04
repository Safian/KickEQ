/*
 * KickEQ - stereo parametric EQ (VST3 + CLAP)
 * Copyright (C) 2026 Safian Szabolcs
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// The worker thread of the linear-phase mode (ARCHITECTURE.md section 4.7). The core itself creates no threads: a
// shell owns one DesignThread per plugin instance, starts it after EqEngine::prepare () (on activate / prepareToPlay),
// stops it before the engine is prepared again or destroyed, and tells it whether the linear-phase mode is wanted.
//
//   - While not active (minimum phase) the thread sleeps on a condition variable: no polling, no work.
//   - While active it polls the request sequence of the DesignLink every few milliseconds and designs the newest
//     parameter snapshot. Designs are at least minInterval apart (leading edge immediate, trailing edge within the
//     interval), so a parameter that is being dragged costs one design every ~12 ms, not one per host block.
//   - Low priority where the platform allows it (nice value / QoS class / thread priority).
//
// The audio thread never waits for this thread and never notifies it (no lock, no system call): everything goes
// through the DesignLink atomics.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "kickeq/config.h"
#include "kickeq/linear_phase.h"

namespace kickeq
{
class DesignThread
{
public:
    /** Minimum time between two designs while parameters keep moving. */
    static constexpr int minIntervalMs = 12;
    /** Polling period while active and idle. */
    static constexpr int pollIntervalMs = 4;

    DesignThread() = default;
    ~DesignThread() { stop(); }

    DesignThread (const DesignThread&) = delete;
    DesignThread& operator= (const DesignThread&) = delete;

    /** Starts the worker for `link` (idle until setActive (true)). The link must outlive the thread. Returns false
        when the thread cannot be created or is already running. Main thread. */
    bool start (DesignLink& link);

    /** Stops and joins the worker (safe to call when not running). Main thread. */
    void stop();

    /** Wakes the worker (true: the linear-phase mode is wanted) or puts it to sleep (false). Any non-audio thread. */
    void setActive (bool active);

    bool running() const noexcept { return thread.joinable(); }

    /** Number of designs completed since start () (for tests and diagnostics). */
    uint64_t designsDone() const noexcept { return done.load (std::memory_order_relaxed); }

private:
    void run();

    DesignLink* link = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    bool wantActive = false; // guarded by mutex
    bool quit = false;       // guarded by mutex
    std::atomic<uint64_t> done { 0 };
};
} // namespace kickeq
