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

// clap_entry of the dynamically loaded KickEQ-native.clap. Compiled into the MODULE target only; the static
// library has no clap_entry so it can be linked next to other CLAP plugins. init does no I/O.

#include <cstring>

#include <clap/clap.h>

#include "kickeq/clap_native.h"

namespace
{
bool CLAP_ABI entryInit (const char*) { return true; }

void CLAP_ABI entryDeinit() {}

const void* CLAP_ABI entryGetFactory (const char* factoryId)
{
    if (factoryId == nullptr)
        return nullptr;
    if (std::strcmp (factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
        return kickeq_get_plugin_factory();
    if (std::strcmp (factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0
        || std::strcmp (factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == 0)
        return kickeq_get_preset_discovery_factory();
    return nullptr;
}
} // namespace

extern "C"
{
// exported through the CLAP_EXPORT declaration in <clap/entry.h> (repeating the attribute on the definition makes
// GCC warn "visibility attribute ignored")
const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entryInit, entryDeinit, entryGetFactory };
}
