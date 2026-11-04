# KickEQ - stereo 8-band parametric EQ (VST3 + CLAP)
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
# JUCE shell (VST3 + CLAP): thin shell over kickeq_core with the editor (ARCHITECTURE.md sections 2.1 and 6)
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
    # kickeq_juce_shell: processor + editor (shared by the plugin and the editor snapshot tool)
    # ---------------------------------------------------------------------------

    add_library(kickeq_juce_shell STATIC
        juce/CoreParameter.h
        juce/Editor.cpp
        juce/Editor.h
        juce/EditorCanvas.cpp
        juce/EditorCanvas.h
        juce/EditorSettings.h
        juce/EditorStrip.cpp
        juce/EditorStrip.h
        juce/EditorTheme.cpp
        juce/EditorTheme.h
        juce/PluginProcessor.cpp
        juce/PluginProcessor.h)

    target_include_directories(kickeq_juce_shell PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/juce")
    # The core headers are written for -Werror builds of the core and the shells; JUCE's recommended warning set
    # (-Wsign-conversion, -Wfloat-equal, ...) is stricter, so consume them as system headers here.
    target_include_directories(kickeq_juce_shell SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core/include")
    target_compile_features(kickeq_juce_shell PUBLIC cxx_std_17)
    target_compile_definitions(kickeq_juce_shell PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        PRIVATE
        KICKEQ_VERSION_STRING="${PROJECT_VERSION}")
    target_link_libraries(kickeq_juce_shell
        PUBLIC
            $<LINK_ONLY:kickeq_core>
            juce::juce_audio_processors
            juce::juce_gui_basics
            juce::juce_graphics
            juce::juce_events
            juce::juce_core
        PRIVATE
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    set_target_properties(kickeq_juce_shell PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # ---------------------------------------------------------------------------
    # Plugin (VST3 here, CLAP through clap-juce-extensions below)
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
        EDITOR_WANTS_KEYBOARD_FOCUS  TRUE
        COPY_PLUGIN_AFTER_BUILD      FALSE
        VST3_CATEGORIES              Fx EQ)

    # Adds the KickEQ_CLAP target (same sources, CLAP wrapper on top of the JUCE processor).
    #  - 64-sample event resolution: the host block is split where parameter events arrive
    #  - CLAP_USE_JUCE_PARAMETER_RANGES ALL: the host sees the plain ranges of ARCHITECTURE.md section 3 (Hz, dB,
    #    enum indices) instead of 0..1 normalised values
    clap_juce_extensions_plugin(
        TARGET        KickEQ
        CLAP_ID       "com.safian.kickeq"
        CLAP_FEATURES audio-effect equalizer stereo
        CLAP_PROCESS_EVENTS_RESOLUTION_SAMPLES 64
        CLAP_USE_JUCE_PARAMETER_RANGES ALL)

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

    target_sources(KickEQ PRIVATE juce/PluginEntry.cpp)

    target_compile_definitions(KickEQ PUBLIC JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(KickEQ
        PRIVATE
            kickeq_juce_shell
            juce::juce_audio_plugin_client
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    # Tests that start JUCE's message manager (or load the JUCE CLAP) get a virtual display on Linux when
    # xvfb-run is installed; without it they need a DISPLAY (JUCE on Linux talks to X11).
    set(KICKEQ_GUI_TEST_RUNNER "")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(KICKEQ_XVFB_RUN xvfb-run)
        if(KICKEQ_XVFB_RUN)
            set(KICKEQ_GUI_TEST_RUNNER "${KICKEQ_XVFB_RUN}" -a)
        endif()
    endif()

    # ---------------------------------------------------------------------------
    # Editor snapshot tool (renders the editor offscreen to PNG)
    # ---------------------------------------------------------------------------

    # Linux CI runners have no display server (JUCE's message manager wants X11): opt in there.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_kickeq_snapshot_default OFF)
    else()
        set(_kickeq_snapshot_default ON)
    endif()
    option(KICKEQ_BUILD_EDITOR_SNAPSHOT "Build kickeq_editor_snapshot (offscreen editor renderer) and its smoke test" ${_kickeq_snapshot_default})

    if(KICKEQ_BUILD_EDITOR_SNAPSHOT)
        juce_add_console_app(kickeq_editor_snapshot PRODUCT_NAME "kickeq_editor_snapshot")
        target_sources(kickeq_editor_snapshot PRIVATE tools/editor_snapshot/main.cpp)
        target_compile_definitions(kickeq_editor_snapshot PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
        target_link_libraries(kickeq_editor_snapshot
            PRIVATE
                kickeq_juce_shell
                juce::juce_recommended_config_flags
                juce::juce_recommended_warning_flags)
        if(KICKEQ_BUILD_TESTS)
            add_test(NAME editor_snapshot_smoke
                COMMAND ${KICKEQ_GUI_TEST_RUNNER} $<TARGET_FILE:kickeq_editor_snapshot> "${CMAKE_BINARY_DIR}/editor_snapshots" --check)
        endif()
    endif()

    # ---------------------------------------------------------------------------
    # Tests of the shell itself (layouts, programs, state chunk, notification, mono audio)
    # ---------------------------------------------------------------------------

    if(KICKEQ_BUILD_TESTS)
        juce_add_console_app(kickeq_juce_shell_test PRODUCT_NAME "kickeq_juce_shell_test")
        target_sources(kickeq_juce_shell_test PRIVATE tools/juce_shell_test/main.cpp)
        target_compile_definitions(kickeq_juce_shell_test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
        target_link_libraries(kickeq_juce_shell_test
            PRIVATE
                kickeq_juce_shell
                juce::juce_recommended_config_flags
                juce::juce_recommended_warning_flags)
        add_test(NAME kickeq_juce_shell_test COMMAND ${KICKEQ_GUI_TEST_RUNNER} $<TARGET_FILE:kickeq_juce_shell_test>)
    endif()

    # ---------------------------------------------------------------------------
    # Cross-shell conformance (needs the tools): JUCE CLAP vs the native CLAP, same core
    # ---------------------------------------------------------------------------

    if(KICKEQ_BUILD_TOOLS AND KICKEQ_BUILD_CLAP_NATIVE AND KICKEQ_BUILD_TESTS AND TARGET clap_testhost)
        # the testhost loads the JUCE CLAP, so building the host builds it as well
        add_dependencies(clap_testhost KickEQ_CLAP)
        set(_kq_dump "${CMAKE_BINARY_DIR}/cross_shell")
        file(MAKE_DIRECTORY "${_kq_dump}")

        # protocol conformance of the JUCE CLAP (profile "juce": see tools/clap_testhost/conformance.cpp)
        add_test(NAME clap_testhost_juce
            COMMAND ${KICKEQ_GUI_TEST_RUNNER} $<TARGET_FILE:clap_testhost> --juce --plugin "$<TARGET_FILE:KickEQ_CLAP>")

        # Bit-exactness: fixed state + 5 s noise at 44.1 and 48 kHz through both shells. The native shell also
        # saves its own blob (--dump-saved); the JUCE shell must load that blob with bit-identical parameter values.
        add_test(NAME cross_shell_native_dump
            COMMAND clap_testhost --baseline-only --plugin "$<TARGET_FILE:KickEQNative>"
                    --dump-state "${_kq_dump}/state.bin" --dump-saved "${_kq_dump}/native-saved.bin"
                    --dump-output "${_kq_dump}/native.f32")
        add_test(NAME cross_shell_juce_dump
            COMMAND ${KICKEQ_GUI_TEST_RUNNER} $<TARGET_FILE:clap_testhost> --baseline-only --juce --plugin "$<TARGET_FILE:KickEQ_CLAP>"
                    --dump-output "${_kq_dump}/juce.f32" --dump-saved "${_kq_dump}/juce-saved.bin")
        add_test(NAME cross_shell_juce_loads_native_state
            COMMAND ${KICKEQ_GUI_TEST_RUNNER} $<TARGET_FILE:clap_testhost> --baseline-only --juce --plugin "$<TARGET_FILE:KickEQ_CLAP>"
                    --state-file "${_kq_dump}/native-saved.bin")
        add_test(NAME cross_shell_native_loads_juce_state
            COMMAND clap_testhost --baseline-only --plugin "$<TARGET_FILE:KickEQNative>"
                    --state-file "${_kq_dump}/juce-saved.bin")
        add_test(NAME cross_shell_compare
            COMMAND clap_testhost --compare "${_kq_dump}/native.f32" "${_kq_dump}/juce.f32")
        set_tests_properties(cross_shell_native_dump PROPERTIES FIXTURES_SETUP kickeq_native_dump)
        set_tests_properties(cross_shell_juce_dump PROPERTIES FIXTURES_SETUP kickeq_juce_dump)
        set_tests_properties(cross_shell_juce_loads_native_state PROPERTIES FIXTURES_REQUIRED kickeq_native_dump)
        set_tests_properties(cross_shell_native_loads_juce_state PROPERTIES FIXTURES_REQUIRED kickeq_juce_dump)
        set_tests_properties(cross_shell_compare PROPERTIES FIXTURES_REQUIRED "kickeq_native_dump;kickeq_juce_dump")
        set_tests_properties(clap_testhost_juce cross_shell_native_dump cross_shell_juce_dump cross_shell_juce_loads_native_state
            cross_shell_native_loads_juce_state cross_shell_compare PROPERTIES TIMEOUT 1800)
    endif()

endif() # KICKEQ_BUILD_JUCE
