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

// Flush-to-zero / denormals-are-zero for the duration of a scope (ARCHITECTURE.md section 4.2). The previous
// floating point control state is restored on destruction, so the host's own state is never changed for
// longer than process(). Architectures without a known control register do nothing.

#pragma once

#include <cstdint>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define KICKEQ_DENORMALS_SSE 1
#elif (defined(__aarch64__) || defined(__arm64__)) && (defined(__GNUC__) || defined(__clang__))
#define KICKEQ_DENORMALS_A64 1
#elif defined(__arm__) && defined(__ARM_FP) && (defined(__GNUC__) || defined(__clang__))
#define KICKEQ_DENORMALS_VFP 1
#endif

namespace kickeq::native
{
class ScopedNoDenormals
{
public:
    ScopedNoDenormals() noexcept
    {
#if defined(KICKEQ_DENORMALS_SSE)
        saved = _mm_getcsr();
        // bit 15 = FTZ; bit 6 = DAZ (SSE2 and later; every x86-64 CPU has it)
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        _mm_setcsr (saved | 0x8040u);
#else
        _mm_setcsr (saved | 0x8000u);
#endif
#elif defined(KICKEQ_DENORMALS_A64)
        uint64_t v = 0;
        __asm__ volatile ("mrs %0, fpcr" : "=r" (v));
        saved = v;
        __asm__ volatile ("msr fpcr, %0" : : "r" (v | (uint64_t (1) << 24))); // FZ
#elif defined(KICKEQ_DENORMALS_VFP)
        uint32_t v = 0;
        __asm__ volatile ("vmrs %0, fpscr" : "=r" (v));
        saved = v;
        __asm__ volatile ("vmsr fpscr, %0" : : "r" (v | (uint32_t (1) << 24))); // FZ
#endif
    }

    ~ScopedNoDenormals()
    {
#if defined(KICKEQ_DENORMALS_SSE)
        _mm_setcsr (saved);
#elif defined(KICKEQ_DENORMALS_A64)
        __asm__ volatile ("msr fpcr, %0" : : "r" (saved));
#elif defined(KICKEQ_DENORMALS_VFP)
        __asm__ volatile ("vmsr fpscr, %0" : : "r" (saved));
#endif
    }

    ScopedNoDenormals (const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator= (const ScopedNoDenormals&) = delete;

private:
#if defined(KICKEQ_DENORMALS_SSE)
    unsigned int saved = 0;
#elif defined(KICKEQ_DENORMALS_A64)
    uint64_t saved = 0;
#elif defined(KICKEQ_DENORMALS_VFP)
    uint32_t saved = 0;
#endif
};
} // namespace kickeq::native
