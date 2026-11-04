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

// Build configuration of the core. KICKEQ_LINEAR_PHASE is set by CMake (option KICKEQ_LINEAR_PHASE, ARCHITECTURE.md
// section 4.7) for kickeq_core and everything that links it; the default below only applies to code that is compiled
// outside the CMake build. The class layouts do not depend on the macro (no ODR hazard), only the availability of the
// linear-phase engine path and of the design thread.

#pragma once

#ifndef KICKEQ_LINEAR_PHASE
#define KICKEQ_LINEAR_PHASE 1
#endif
