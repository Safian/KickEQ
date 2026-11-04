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

// Minimal header-only test runner: TEST (name) { CHECK (...); } registers a case, main.cpp runs them.

#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ktest
{
struct Case
{
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry()
{
    static std::vector<Case> cases;
    return cases;
}

struct State
{
    int checks = 0;
    int failedChecks = 0;
    int failedInCase = 0;
};

inline State& state()
{
    static State s;
    return s;
}

struct Registrar
{
    Registrar (const char* name, void (*fn)()) { registry().push_back ({ name, fn }); }
};

inline void report (bool ok, const char* expr, const char* file, int line)
{
    auto& s = state();
    ++s.checks;
    if (!ok)
    {
        ++s.failedChecks;
        ++s.failedInCase;
        std::printf ("    FAILED %s:%d: %s\n", file, line, expr);
    }
}

inline void reportNear (double a, double b, double tol, const char* sa, const char* sb, const char* file, int line)
{
    const bool ok = std::fabs (a - b) <= tol; // NaN fails
    auto& s = state();
    ++s.checks;
    if (!ok)
    {
        ++s.failedChecks;
        ++s.failedInCase;
        std::printf ("    FAILED %s:%d: %s (%.9g) ~ %s (%.9g), tolerance %.3g\n", file, line, sa, a, sb, b, tol);
    }
}
} // namespace ktest

#define KTEST_CAT2(a, b) a##b
#define KTEST_CAT(a, b) KTEST_CAT2 (a, b)

#define TEST(name)                                                                         \
    static void name();                                                                    \
    static ::ktest::Registrar KTEST_CAT (registrar_, name) (#name, name);                  \
    static void name()

#define CHECK(cond) ::ktest::report (static_cast<bool> (cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) ::ktest::reportNear ((a), (b), (tol), #a, #b, __FILE__, __LINE__)
