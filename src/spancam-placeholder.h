/*
Spancam for OBS
Copyright (C) 2026 Aditya Dewaskar <support@adewaskar.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <obs-module.h>
#include <stdbool.h>

/*
 * The card the source shows instead of a black rectangle when there is no phone:
 * a QR to the download page plus the four setup steps.
 *
 * A source with no picture is indistinguishable from a broken one. OBS renders an
 * async source with no frame as nothing at all, so a first-time user adds "Spancam
 * Camera", sees an empty rectangle, and has nothing to act on. Feeding the setup
 * instructions AS the video puts the answer exactly where they are already looking.
 *
 * The artwork is baked (tools/make-placeholder.py) rather than drawn at runtime: drawing
 * it in C would mean shipping a font rasteriser and a text layout engine to render one
 * static card.
 */

/* Loads the asset once. Cheap and idempotent on later calls. Returns false when the
 * asset is missing or malformed, and the source then simply has no picture as before —
 * a bad placeholder must never be worse than no placeholder. */
bool spancam_placeholder_init(void);

void spancam_placeholder_free(void);

/* Pushes the card as this source's current frame. No-op when the asset failed to load. */
void spancam_placeholder_output(obs_source_t *source);

/* True when the card on screen was composed for a different canvas than the one now
 * configured — i.e. the user changed resolution or flipped orientation while the card was
 * showing, and it needs re-composing. Cheap: two integer compares. */
bool spancam_placeholder_stale(void);
