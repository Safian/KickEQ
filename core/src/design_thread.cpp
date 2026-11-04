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

#include "kickeq/design_thread.h"

#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace kickeq
{
namespace
{
/** The design is background work: below the audio thread and the UI. Failures are ignored. */
void lowerPriorityOfThisThread() noexcept
{
#if defined(_WIN32)
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
#elif defined(__linux__) || defined(__ANDROID__)
    // nice values are per thread on Linux: lower only this one
    (void) setpriority (PRIO_PROCESS, static_cast<id_t> (syscall (SYS_gettid)), 10);
#endif
}
} // namespace

bool DesignThread::start (DesignLink& l)
{
    if (thread.joinable())
        return false;
    link = &l;
    done.store (0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock (mutex);
        quit = false;
        wantActive = false;
    }
    thread = std::thread ([this] { run(); });
    return thread.joinable();
}

void DesignThread::stop()
{
    if (!thread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock (mutex);
        quit = true;
    }
    wake.notify_all();
    thread.join();
    link = nullptr;
}

void DesignThread::setActive (bool active)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        wantActive = active;
    }
    wake.notify_all();
}

void DesignThread::run()
{
    lowerPriorityOfThisThread();

    LinearPhaseDesigner designer; // allocated here, on the worker
    uint64_t lastDesigned = 0;
    auto lastStart = std::chrono::steady_clock::now() - std::chrono::milliseconds (minIntervalMs);

    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock (mutex);
            wake.wait (lock, [this] { return quit || wantActive; });
            if (quit)
                return;
        }

        if (link->requestSequence() != lastDesigned)
        {
            // throttle: the next design starts no earlier than minInterval after the previous one started
            const auto earliest = lastStart + std::chrono::milliseconds (minIntervalMs);
            const auto now = std::chrono::steady_clock::now();
            if (now < earliest)
            {
                std::unique_lock<std::mutex> lock (mutex);
                if (wake.wait_for (lock, earliest - now, [this] { return quit || !wantActive; }))
                {
                    if (quit)
                        return;
                    continue; // went to sleep meanwhile
                }
            }

            lastStart = std::chrono::steady_clock::now();
            const uint64_t handled = serviceDesignRequest (*link, designer, lastDesigned);
            if (handled != lastDesigned)
            {
                lastDesigned = handled;
                done.fetch_add (1, std::memory_order_relaxed);
            }
        }
        else
        {
            std::unique_lock<std::mutex> lock (mutex);
            if (wake.wait_for (lock, std::chrono::milliseconds (pollIntervalMs), [this] { return quit || !wantActive; }) && quit)
                return;
        }
    }
}
} // namespace kickeq
