# KickEQ - CMake toolchain file
# Copyright (C) 2026 Safian Szabolcs
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Akai MPC Key 61 (Linux ARMv7, hard-float, NEON). Needs gcc-arm-linux-gnueabihf / g++-arm-linux-gnueabihf.
# Usage: cmake -S . -B build-armv7 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/armv7-linux-gnueabihf.cmake -DKICKEQ_BUILD_JUCE=OFF
# The linear-phase mode (FFT convolution + design thread) is OFF by default for the MPC until the hardware benchmark shows
# it fits (ARCHITECTURE.md section 4.7); a cache default, so -DKICKEQ_LINEAR_PHASE=ON on the command line wins.
set(KICKEQ_LINEAR_PHASE OFF CACHE BOOL "Build the linear-phase mode (phase_mode parameter, FFT convolver, design thread)")
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_C_FLAGS_INIT   "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
# Tools run on the MPC image: do not depend on its libstdc++ / libgcc.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Run test binaries under qemu-user (apt: qemu-user): ctest picks this up.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-arm -L /usr/arm-linux-gnueabihf)
