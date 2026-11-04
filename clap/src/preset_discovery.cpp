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

// clap.preset-discovery: one provider that declares the factory presets as a "plugin" location. The presets are
// loaded through clap.preset-load (Plugin::presetFromLocation); the load key is the preset name.

#include <cstring>
#include <new>

#include <clap/clap.h>

#include "kickeq/clap_native.h"
#include "kickeq/presets.h"
#include "plugin.h"

namespace kickeq::native
{
namespace
{
constexpr const char* providerId = "com.safian.kickeq/factory-presets";

const clap_preset_discovery_provider_descriptor_t providerDescriptor = {
    CLAP_VERSION_INIT, providerId, "KickEQ factory presets", "Safian"
};

struct Provider
{
    clap_preset_discovery_provider_t base;
    const clap_preset_discovery_indexer_t* indexer;
};

const Provider& self (const clap_preset_discovery_provider_t* p) { return *static_cast<const Provider*> (p->provider_data); }

bool CLAP_ABI providerInit (const clap_preset_discovery_provider_t* p)
{
    const clap_preset_discovery_indexer_t* ix = self (p).indexer;
    if (ix == nullptr || ix->declare_location == nullptr)
        return false;

    clap_preset_discovery_location_t loc {};
    loc.flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT;
    loc.name = "KickEQ factory presets";
    loc.kind = CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
    loc.location = nullptr; // plugin kind: must be null
    return ix->declare_location (ix, &loc);
}

void CLAP_ABI providerDestroy (const clap_preset_discovery_provider_t* p) { delete &self (p); }

bool CLAP_ABI providerGetMetadata (const clap_preset_discovery_provider_t*, uint32_t locationKind, const char*,
                                   const clap_preset_discovery_metadata_receiver_t* r)
{
    if (locationKind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || r == nullptr || r->begin_preset == nullptr)
        return false;

    const clap_universal_plugin_id_t plugin = { "clap", pluginDescriptor()->id };
    for (int i = 0; i < numPresets; ++i)
    {
        const char* name = factoryPresets[i].name;
        if (!r->begin_preset (r, name, name))
            return true; // the receiver asked to stop
        if (r->add_plugin_id != nullptr)
            r->add_plugin_id (r, &plugin);
        if (r->set_flags != nullptr)
            r->set_flags (r, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
        if (r->add_creator != nullptr)
            r->add_creator (r, "Safian");
        if (r->add_feature != nullptr)
            r->add_feature (r, "equalizer");
    }
    return true;
}

const void* CLAP_ABI providerGetExtension (const clap_preset_discovery_provider_t*, const char*) { return nullptr; }

uint32_t CLAP_ABI factoryCount (const clap_preset_discovery_factory_t*) { return 1; }

const clap_preset_discovery_provider_descriptor_t* CLAP_ABI factoryDescriptor (const clap_preset_discovery_factory_t*,
                                                                              uint32_t index)
{
    return index == 0 ? &providerDescriptor : nullptr;
}

const clap_preset_discovery_provider_t* CLAP_ABI factoryCreate (const clap_preset_discovery_factory_t*,
                                                               const clap_preset_discovery_indexer_t* indexer,
                                                               const char* id)
{
    if (indexer == nullptr || id == nullptr || std::strcmp (id, providerId) != 0)
        return nullptr;

    Provider* p = new (std::nothrow) Provider {};
    if (p == nullptr)
        return nullptr;
    p->base.desc = &providerDescriptor;
    p->base.provider_data = p;
    p->base.init = &providerInit;
    p->base.destroy = &providerDestroy;
    p->base.get_metadata = &providerGetMetadata;
    p->base.get_extension = &providerGetExtension;
    p->indexer = indexer;
    return &p->base;
}

const clap_preset_discovery_factory_t presetFactory = { &factoryCount, &factoryDescriptor, &factoryCreate };
} // namespace
} // namespace kickeq::native

extern "C"
{
const clap_preset_discovery_factory_t* kickeq_get_preset_discovery_factory (void) { return &kickeq::native::presetFactory; }
}
