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

// Internal interface between the plugin implementation and the factories (not installed).

#pragma once

#include <clap/plugin.h>

namespace kickeq::native
{
/** Static descriptor of the one plugin (id com.safian.kickeq). */
const clap_plugin_descriptor_t* pluginDescriptor() noexcept;

/** New plugin instance for `host` (version-checked by the caller); nullptr on failure. The host owns it and
    ends it with plugin->destroy. */
const clap_plugin_t* createPlugin (const clap_host_t* host) noexcept;

/** Table index (0 .. numPresets - 1) of a preset load key (the preset name, or its decimal index); -1 when
    unknown. */
int presetIndexForLoadKey (const char* loadKey) noexcept;
} // namespace kickeq::native
