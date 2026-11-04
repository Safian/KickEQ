# kickeq_core unit tests (own minimal runner, no external framework)
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

find_package(Threads REQUIRED)

add_executable(kickeq_core_tests
    main.cpp
    test_analyzer.cpp
    test_autogain.cpp
    test_blocksize.cpp
    test_extremes.cpp
    test_fft.cpp
    test_linear_phase.cpp
    test_null.cpp
    test_params.cpp
    test_presets.cpp
    test_response.cpp
    test_smoothing.cpp
    test_state.cpp
    test_stereo.cpp)
target_link_libraries(kickeq_core_tests PRIVATE kickeq_core Threads::Threads)

if(MSVC)
    target_compile_options(kickeq_core_tests PRIVATE /W4 /WX)
else()
    target_compile_options(kickeq_core_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off)
endif()

add_test(NAME kickeq_core_tests COMMAND kickeq_core_tests)
set_tests_properties(kickeq_core_tests PROPERTIES TIMEOUT 1200)

# Linear-phase mode: heap-free audio side (global operator new counting) and the threaded hand-over (also run under
# ThreadSanitizer by hand: cmake -DCMAKE_CXX_FLAGS=-fsanitize=thread)
if(KICKEQ_LINEAR_PHASE)
    foreach(t kickeq_linear_alloc_test kickeq_linear_thread_test)
        string(REPLACE "kickeq_linear_" "" src ${t})
        add_executable(${t} ${src}.cpp)
        target_link_libraries(${t} PRIVATE kickeq_core Threads::Threads)
        if(MSVC)
            target_compile_options(${t} PRIVATE /W4 /WX)
        else()
            target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off)
        endif()
        add_test(NAME ${t} COMMAND ${t})
        set_tests_properties(${t} PROPERTIES TIMEOUT 600)
    endforeach()
endif()
