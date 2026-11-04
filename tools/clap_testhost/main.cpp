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

// clap_testhost - conformance host for the native CLAP shell.
//
//   clap_testhost --static                    link kickeq_clap_native, use kickeq_get_plugin_factory (StaticPluginLoader path)
//   clap_testhost --plugin KickEQ-native.clap load the .clap with dlopen / LoadLibrary (DynamicPluginLoader path)
//   options: --dump-output FILE.f32   write the baseline output (5 s stereo noise through the fixed state, first at
//                                     44.1 kHz then at 48 kHz, interleaved little endian float32) for a later
//                                     comparison with another shell
//            --dump-state FILE        write the fixed baseline state blob
//            --no-threads             skip the section that starts a second thread (qemu-user on some hosts crashes
//                                     in pthread_create; the section is reported as SKIP)
// Exit code 0 when every check passed.

#include <cstdio>
#include <cstring>
#include <string>

#include "testhost.h"

int main (int argc, char** argv)
{
    bool useStatic = false;
    std::string pluginPath;
    th::Options options;

    for (int i = 1; i < argc; ++i)
    {
        const auto arg = [&] (const char* name) { return std::strcmp (argv[i], name) == 0 && i + 1 < argc; };
        if (std::strcmp (argv[i], "--static") == 0)
            useStatic = true;
        else if (std::strcmp (argv[i], "--no-threads") == 0)
            options.noThreads = true;
        else if (arg ("--plugin"))
            pluginPath = argv[++i];
        else if (arg ("--dump-output"))
            options.dumpOutput = argv[++i];
        else if (arg ("--dump-state"))
            options.dumpState = argv[++i];
        else
        {
            std::fprintf (stderr, "usage: %s (--static | --plugin PATH.clap) [--no-threads] [--dump-output FILE.f32] [--dump-state FILE]\n", argv[0]);
            return 2;
        }
    }
    if (useStatic == !pluginPath.empty())
    {
        std::fprintf (stderr, "exactly one of --static and --plugin PATH is required\n");
        return 2;
    }

    th::Loader loader;
    if (useStatic)
    {
        std::printf ("clap_testhost: static mode (kickeq_get_plugin_factory)\n");
        if (!loader.openStatic())
        {
            std::fprintf (stderr, "static factories are not available\n");
            return 1;
        }
    }
    else
    {
        std::printf ("clap_testhost: dynamic mode (%s)\n", pluginPath.c_str());
        std::string error;
        if (!loader.openDynamic (pluginPath, error))
        {
            std::fprintf (stderr, "cannot load %s: %s\n", pluginPath.c_str(), error.c_str());
            return 1;
        }
    }
    return th::runConformance (loader, options);
}
