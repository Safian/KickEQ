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

/* Static-loader entry points of the native CLAP shell (ARCHITECTURE.md section 10). Linking
 * libkickeq_clap_native.a and calling these needs no dlopen; the dynamic KickEQ-native.clap returns the very
 * same factories from clap_entry.get_factory. */

#ifndef KICKEQ_CLAP_NATIVE_H
#define KICKEQ_CLAP_NATIVE_H

#include <clap/factory/plugin-factory.h>
#include <clap/factory/preset-discovery.h>
#include <clap/private/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The plugin factory (one plugin, id "com.safian.kickeq"). Never NULL, static storage, no init needed. */
CLAP_EXPORT const clap_plugin_factory_t *kickeq_get_plugin_factory(void);

/* The preset discovery factory (one provider declaring the factory presets). Never NULL. */
CLAP_EXPORT const clap_preset_discovery_factory_t *kickeq_get_preset_discovery_factory(void);

#ifdef __cplusplus
}
#endif

#endif /* KICKEQ_CLAP_NATIVE_H */
