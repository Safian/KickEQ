# KickEQ - stereo 4-band parametric EQ (VST3 + CLAP from one code base)
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

cmake_minimum_required(VERSION 3.22)

# Must be set before project(). macOS builds target the host architecture by default
# (arm64 on Apple Silicon); pass -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" for a universal build.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")

# Build configuration. KICKEQ_BUILD_JUCE=OFF needs nothing but CMake and a C++17 compiler (the cross builds
# and the portable core CI jobs use it); the JUCE/clap-juce-extensions dependencies are then never fetched.
option(KICKEQ_BUILD_JUCE  "Build the JUCE plugin shell (VST3 + CLAP)" ON)
option(KICKEQ_BUILD_CLAP_NATIVE "Build the native CLAP shell (static library kickeq_clap_native + KickEQ-native.clap)" ON)
option(KICKEQ_BUILD_TOOLS "Build the developer tools (clap_testhost conformance host, kickeq_bench) and register their tests" ON)
option(KICKEQ_BUILD_TESTS "Build and register the kickeq_core unit tests" ON)
option(KICKEQ_SANITIZE    "Build kickeq_core and its tests with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

project(KickEQ VERSION 0.1.0 LANGUAGES CXX)
if(KICKEQ_BUILD_JUCE)
    enable_language(C)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Portable DSP core (no dependencies) and its tests
# ---------------------------------------------------------------------------

if(KICKEQ_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

# ---------------------------------------------------------------------------
# Native CLAP shell (no GUI toolkit; depends on the CLAP headers and kickeq_core only)
# ---------------------------------------------------------------------------

if(KICKEQ_BUILD_CLAP_NATIVE)
    add_subdirectory(clap)
endif()

if(KICKEQ_BUILD_TOOLS)
    if(NOT KICKEQ_BUILD_CLAP_NATIVE)
        message(STATUS "KickEQ: KICKEQ_BUILD_TOOLS needs KICKEQ_BUILD_CLAP_NATIVE, tools are skipped")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(STATUS "KickEQ: no tools on iOS (static library only)")
    else()
        add_subdirectory(tools)
    endif()
endif()

# ---------------------------------------------------------------------------
# Plugin manifest (ARCHITECTURE.md section 10); the release job fills in "builds"
# ---------------------------------------------------------------------------

set(KICKEQ_LICENSE_SPDX "AGPL-3.0-or-later")
set(KICKEQ_MIN_HOST_VERSION "1.0.0")
configure_file(manifest/kickeq.manifest.json.in "${CMAKE_BINARY_DIR}/manifest/kickeq.manifest.json" @ONLY)

include(GNUInstallDirs)
install(FILES "${CMAKE_BINARY_DIR}/manifest/kickeq.manifest.json" DESTINATION "${CMAKE_INSTALL_DATADIR}/kickeq")

# ---------------------------------------------------------------------------
# JUCE shell (VST3 + CLAP)
# ---------------------------------------------------------------------------

if(KICKEQ_BUILD_JUCE)

    include(FetchContent)

    # ---------------------------------------------------------------------------
    # Dependencies - exact versions are pinned (see THIRD_PARTY.md).
    # ---------------------------------------------------------------------------

    # JUCE, used under its AGPL-3.0 option. 8.0.17-security is the newest 8.x release.
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.17-security
        GIT_SHALLOW    TRUE)

    # clap-juce-extensions (MIT). The project does not publish recent tags (the newest one,
    # 0.26.0, dates from 2022), so a commit on its main branch is pinned instead.
    FetchContent_Declare(clap-juce-extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        GIT_TAG        7adee3a1bd4684d4caa5601100e364abccff4b4d
        GIT_SUBMODULES_RECURSE TRUE)

    FetchContent_MakeAvailable(JUCE clap-juce-extensions)

    # ---------------------------------------------------------------------------
    # Plugin
    # ---------------------------------------------------------------------------

    juce_add_plugin(KickEQ
        COMPANY_NAME                 "Safian"
        BUNDLE_ID                    "com.safian.kickeq"
        PLUGIN_MANUFACTURER_CODE     Safn
        PLUGIN_CODE                  Keeq
        FORMATS                      VST3
        PRODUCT_NAME                 "KickEQ"
        IS_SYNTH                     FALSE
        NEEDS_MIDI_INPUT             FALSE
        NEEDS_MIDI_OUTPUT            FALSE
        IS_MIDI_EFFECT               FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS  FALSE
        COPY_PLUGIN_AFTER_BUILD      FALSE
        VST3_CATEGORIES              Fx EQ)

    # Adds the KickEQ_CLAP target (same sources, CLAP wrapper on top of the JUCE processor).
    clap_juce_extensions_plugin(
        TARGET        KickEQ
        CLAP_ID       "com.safian.kickeq"
        CLAP_FEATURES audio-effect equalizer stereo
        # split the host block at event boundaries (sample-accurate automation, 64-sample grid)
        CLAP_PROCESS_EVENTS_RESOLUTION_SAMPLES 64)

    if(APPLE)
        # JUCE ad-hoc signs the VST3 bundle, but then writes Resources/moduleinfo.json, which invalidates
        # the seal on a fresh build; the CLAP bundle is only linker-signed (invalid once Info.plist and
        # resources are added). Re-sign both as the last post-build step so they verify with `codesign`.
        # Use -DKICKEQ_CODESIGN_IDENTITY="Developer ID Application: ..." to sign for real.
        set(KICKEQ_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity for the plugin bundles (- = ad hoc)")
        foreach(bundle_target KickEQ_VST3 KickEQ_CLAP)
            add_custom_command(TARGET ${bundle_target} POST_BUILD
                COMMAND codesign --force --sign "${KICKEQ_CODESIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:${bundle_target}>"
                VERBATIM)
        endforeach()
    endif()

    target_sources(KickEQ PRIVATE
        Source/Biquad.h
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h)

    target_compile_definitions(KickEQ PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(KickEQ
        PRIVATE
            juce::juce_audio_processors
            juce::juce_audio_plugin_client
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

endif() # KICKEQ_BUILD_JUCE

