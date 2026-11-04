# Developer tools: clap_testhost (conformance host) and kickeq_bench (CPU benchmark)
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

function(kickeq_tool_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off)
    endif()
endfunction()

# --- conformance host --------------------------------------------------------------------------------------

add_executable(clap_testhost
    clap_testhost/main.cpp
    clap_testhost/testhost.cpp
    clap_testhost/conformance.cpp)
target_link_libraries(clap_testhost PRIVATE kickeq_clap_native Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(clap_testhost PRIVATE KICKEQ_EXPECTED_VERSION="${PROJECT_VERSION}")
kickeq_tool_warnings(clap_testhost)
# the dynamic test needs the module: building the host builds the .clap as well
add_dependencies(clap_testhost KickEQNative)

# --- benchmark ---------------------------------------------------------------------------------------------

add_executable(kickeq_bench bench/bench.cpp)
target_link_libraries(kickeq_bench PRIVATE kickeq_clap_native)
kickeq_tool_warnings(kickeq_bench)

# --- ctest -------------------------------------------------------------------------------------------------

if(KICKEQ_BUILD_TESTS)
    add_test(NAME clap_testhost_static COMMAND clap_testhost --static)
    add_test(NAME clap_testhost_dynamic COMMAND clap_testhost --plugin "$<TARGET_FILE:KickEQNative>")
    add_test(NAME kickeq_bench_check COMMAND kickeq_bench --check)
    set_tests_properties(clap_testhost_static clap_testhost_dynamic kickeq_bench_check PROPERTIES TIMEOUT 1800)
endif()
