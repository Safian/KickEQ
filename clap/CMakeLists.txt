# kickeq_clap_native - native CLAP shell of KickEQ (no GUI toolkit; CLAP headers + kickeq_core only)
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

include(FetchContent)

# CLAP headers (MIT), pinned by tag. Offline builds: -DFETCHCONTENT_SOURCE_DIR_CLAP=<path to a checkout of 1.2.7>.
# SOURCE_SUBDIR points to a directory without a CMakeLists.txt so the upstream project is only downloaded, not
# added: its `clap` target would collide with the one of clap-juce-extensions when the JUCE shell is built too.
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG        1.2.7
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  kickeq-no-cmake)
FetchContent_MakeAvailable(clap)

add_library(kickeq_clap_headers INTERFACE)
target_include_directories(kickeq_clap_headers SYSTEM INTERFACE "${clap_SOURCE_DIR}/include")

# ---------------------------------------------------------------------------
# Static library: the plugin, the factories and the static-loader entry points
# ---------------------------------------------------------------------------

add_library(kickeq_clap_native STATIC
    src/factory.cpp
    src/plugin.cpp
    src/preset_discovery.cpp)
add_library(kickeq::clap_native ALIAS kickeq_clap_native)

target_include_directories(kickeq_clap_native PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(kickeq_clap_native PUBLIC kickeq_core kickeq_clap_headers)
target_compile_features(kickeq_clap_native PUBLIC cxx_std_17)
target_compile_definitions(kickeq_clap_native PRIVATE
    KICKEQ_VERSION_STRING="${PROJECT_VERSION}"
    $<$<CONFIG:Debug>:KICKEQ_THREAD_CHECKS=1>)
set_target_properties(kickeq_clap_native PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

if(MSVC)
    target_compile_options(kickeq_clap_native PRIVATE /W4 /WX /GR- /fp:precise)
    target_compile_definitions(kickeq_clap_native PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(kickeq_clap_native PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -fno-exceptions -fno-rtti -ffp-contract=off)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS") # iOS: static library only, apps cannot load downloaded code

# ---------------------------------------------------------------------------
# Loadable module: KickEQ-native.clap
# ---------------------------------------------------------------------------

add_library(KickEQNative MODULE src/entry.cpp)
target_link_libraries(KickEQNative PRIVATE kickeq_clap_native)
set_target_properties(KickEQNative PROPERTIES
    OUTPUT_NAME KickEQ-native
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native")

if(MSVC)
    target_compile_options(KickEQNative PRIVATE /W4 /WX /GR-)
else()
    target_compile_options(KickEQNative PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
endif()

if(APPLE)
    set(KICKEQ_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity for the plugin bundles (- = ad hoc)")
    set(MACOSX_BUNDLE_EXECUTABLE_NAME "KickEQ-native")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.safian.kickeq.native")
    set(MACOSX_BUNDLE_BUNDLE_NAME "KickEQ")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    set_target_properties(KickEQNative PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION clap
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in")
    # only clap_entry leaves the bundle
    target_link_options(KickEQNative PRIVATE "LINKER:-exported_symbol,_clap_entry")
    add_custom_command(TARGET KickEQNative POST_BUILD
        COMMAND codesign --force --sign "${KICKEQ_CODESIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:KickEQNative>"
        VERBATIM)
else()
    set_target_properties(KickEQNative PROPERTIES PREFIX "" SUFFIX ".clap")
    if(UNIX)
        # only clap_entry is exported (the static libraries linked into the module stay private)
        target_link_options(KickEQNative PRIVATE
            "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/linux-export.map")
        # A downloaded plugin must not depend on the libstdc++ of the host image (MPC): link it in.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT KICKEQ_SANITIZE)
            target_link_options(KickEQNative PRIVATE -static-libstdc++ -static-libgcc)
        endif()
    endif()
endif()

endif() # NOT iOS

# ---------------------------------------------------------------------------
# Install: the .clap, the static libraries, the vendor extension header
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
if(TARGET KickEQNative)
    install(TARGETS KickEQNative
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/clap"
        RUNTIME DESTINATION "${CMAKE_INSTALL_LIBDIR}/clap"
        BUNDLE  DESTINATION "${CMAKE_INSTALL_LIBDIR}/clap")
endif()
install(TARGETS kickeq_clap_native kickeq_core ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES include/kickeq/clap_analyzer.h include/kickeq/clap_native.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/kickeq")
