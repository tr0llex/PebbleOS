/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "applib/graphics/graphics.h"
#include "applib/graphics/text.h"

//! Draw `text` centred both ways inside `band`.
//!
//! graphics_draw_text() puts the top of the line box at the top of the given
//! rectangle, and the line box carries the font's leading above the glyphs. So
//! centring the box leaves the ink sitting low. Measuring the line and taking
//! an eighth of it back off the top is the correction that lined the ink up
//! with the measured centre on this display; it is the same figure the
//! watchface uses.
static inline void app_draw_text_centred(GContext *ctx, const char *text, GFont const font,
                                         GRect band) {
  const GSize size = graphics_text_layout_get_max_used_size(
      ctx, text, font, band, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  GRect box = band;
  box.origin.y += (band.size.h - size.h) / 2 - size.h / 8;
  box.size.h = size.h + band.size.h; // room for the upward shift, so nothing clips
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     NULL);
}
