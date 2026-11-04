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

/* Vendor extension "com.safian.kickeq/analyzer/1" (ARCHITECTURE.md section 5.3).
 *
 * Lets a UI that is not the plugin's own (Kick on the MPC and on phones) draw the spectrum of the native
 * shell without clap.gui. Obtain it with plugin->get_extension (plugin, KICKEQ_CLAP_EXT_ANALYZER).
 *
 * Threading: every function is MAIN / UI THREAD ONLY. The audio thread only pushes samples into wait-free
 * rings while the analyser is enabled and the plugin is in realtime render mode (it stays idle in offline
 * mode). The analyser is allocated lazily by the first set_enabled (true) and lives until the plugin is
 * destroyed. Plain C so that C hosts can use it.
 */

#ifndef KICKEQ_CLAP_ANALYZER_H
#define KICKEQ_CLAP_ANALYZER_H

#include <clap/plugin.h>

#define KICKEQ_CLAP_EXT_ANALYZER "com.safian.kickeq/analyzer/1"

/* Number of log-spaced display bins (10 Hz .. 30 kHz); constant, valid before the analyser is enabled. */
#define KICKEQ_CLAP_ANALYZER_NUM_BINS 512

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kickeq_clap_plugin_analyzer {
   /* Enables or disables the analyser. The first call with enabled = true allocates it (and may be slow). */
   void (CLAP_ABI *set_enabled)(const clap_plugin_t *plugin, bool enabled);

   /* Number of display bins, always KICKEQ_CLAP_ANALYZER_NUM_BINS. */
   uint32_t (CLAP_ABI *num_bins)(const clap_plugin_t *plugin);

   /* Centre frequency in Hz of display bin 'bin' (0 .. num_bins - 1); 0 for an index out of range. */
   float (CLAP_ABI *bin_frequency_hz)(const clap_plugin_t *plugin, uint32_t bin);

   /* Services the analyser (consumes the pushed audio) and copies the latest spectrum frame in dBFS
    * (floor -200) into pre_db (input) and post_db (output); either pointer may be NULL. n must be equal to
    * num_bins(). Returns false when n is wrong, the analyser was never enabled, or no frame has been
    * produced yet. Once a frame exists, further calls return the newest one (repeated if nothing new). */
   bool (CLAP_ABI *get_frame)(const clap_plugin_t *plugin, float *pre_db, float *post_db, uint32_t n);
} kickeq_clap_plugin_analyzer_t;

#ifdef __cplusplus
}
#endif

#endif /* KICKEQ_CLAP_ANALYZER_H */
