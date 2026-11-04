# kickeq_core - portable DSP core of KickEQ (static library, C++17, standard library only)
# Copyright (C) 2026 Safian Szabolcs
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

add_library(kickeq_core STATIC
    src/analyzer.cpp
    src/eq_curve.cpp
    src/eq_engine.cpp
    src/fft.cpp
    src/params.cpp
    src/state.cpp)
add_library(kickeq::core ALIAS kickeq_core)

target_include_directories(kickeq_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(kickeq_core PUBLIC cxx_std_17)
set_target_properties(kickeq_core PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)   # the static library is also linked into the .clap modules

# Warnings are errors for the core. No exceptions, no RTTI, no fast-math; floating point contraction (FMA)
# is disabled so the results do not depend on the target's fused multiply-add support.
if(MSVC)
    target_compile_options(kickeq_core PRIVATE /W4 /WX /GR- /fp:precise)
else()
    target_compile_options(kickeq_core PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -fno-exceptions -fno-rtti -ffp-contract=off)
endif()

if(KICKEQ_SANITIZE)
    if(MSVC)
        target_compile_options(kickeq_core PUBLIC /fsanitize=address)
    else()
        target_compile_options(kickeq_core PUBLIC
            -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(kickeq_core PUBLIC -fsanitize=address,undefined)
    endif()
endif()

if(KICKEQ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
