# KickEQ - CMake toolchain file
# Copyright (C) 2026 Safian Szabolcs
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# iOS static-library build check (core + native CLAP shell only; no JUCE). Use with the Xcode or Ninja
# generator on macOS: cmake -S . -B build-ios -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake
# -DKICKEQ_IOS_SDK=iphoneos|iphonesimulator (default iphoneos)
set(CMAKE_SYSTEM_NAME iOS)
if(NOT DEFINED KICKEQ_IOS_SDK)
    set(KICKEQ_IOS_SDK iphoneos)
endif()
set(CMAKE_OSX_SYSROOT ${KICKEQ_IOS_SDK})
set(CMAKE_OSX_ARCHITECTURES arm64)
if(NOT DEFINED KICKEQ_IOS_DEPLOYMENT_TARGET)
    set(KICKEQ_IOS_DEPLOYMENT_TARGET 16.0)
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET ${KICKEQ_IOS_DEPLOYMENT_TARGET})
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
