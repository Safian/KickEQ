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

// The plugin factory and the static-loader entry points (ARCHITECTURE.md section 10).

#include <cstring>

#include <clap/clap.h>

#include "kickeq/clap_native.h"
#include "plugin.h"

namespace kickeq::native
{
namespace
{
uint32_t CLAP_ABI getPluginCount (const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* CLAP_ABI getPluginDescriptor (const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? pluginDescriptor() : nullptr;
}

const clap_plugin_t* CLAP_ABI createPluginInstance (const clap_plugin_factory_t*, const clap_host_t* host,
                                                     const char* pluginId)
{
    if (host == nullptr || pluginId == nullptr || !clap_version_is_compatible (host->clap_version)
        || std::strcmp (pluginId, pluginDescriptor()->id) != 0)
        return nullptr;
    return createPlugin (host);
}

const clap_plugin_factory_t pluginFactory = { &getPluginCount, &getPluginDescriptor, &createPluginInstance };
} // namespace
} // namespace kickeq::native

extern "C"
{
const clap_plugin_factory_t* kickeq_get_plugin_factory (void) { return &kickeq::native::pluginFactory; }
}
