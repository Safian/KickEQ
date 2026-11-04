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

#include <chrono>
#include <cstdio>
#include <cstring>

#include "test.h"

// usage: kickeq_core_tests [substring-of-test-name]
int main (int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed = 0;

    for (const auto& c : ktest::registry())
    {
        if (filter != nullptr && std::strstr (c.name, filter) == nullptr)
            continue;

        ktest::state().failedInCase = 0;
        const auto t0 = std::chrono::steady_clock::now();
        c.fn();
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();

        ++run;
        if (ktest::state().failedInCase != 0)
            ++failed;
        std::printf ("[%s] %s (%.2f s)\n", ktest::state().failedInCase == 0 ? " ok " : "FAIL", c.name, sec);
    }

    std::printf ("\n%d tests, %d checks, %d tests failed, %d checks failed\n", run, ktest::state().checks, failed,
                 ktest::state().failedChecks);
    return failed == 0 && run > 0 ? 0 : 1;
}
