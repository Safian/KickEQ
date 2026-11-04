# KickEQ - CMake toolchain file
# Copyright (C) 2026 Safian Szabolcs
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Android static-library build check. Wraps the NDK toolchain file; set ANDROID_NDK (or ANDROID_NDK_HOME)
# and optionally -DANDROID_ABI=arm64-v8a|armeabi-v7a (default arm64-v8a).
if(NOT DEFINED ANDROID_NDK)
    if(DEFINED ENV{ANDROID_NDK_HOME})
        set(ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
    elseif(DEFINED ENV{ANDROID_NDK_LATEST_HOME})
        set(ANDROID_NDK "$ENV{ANDROID_NDK_LATEST_HOME}")
    else()
        message(FATAL_ERROR "Set ANDROID_NDK or ANDROID_NDK_HOME")
    endif()
endif()
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI arm64-v8a)
endif()
set(ANDROID_PLATFORM android-24)
set(ANDROID_STL c++_static)
include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
