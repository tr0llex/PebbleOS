/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "compass.h"

#ifdef CONFIG_APP_COMPASS

#include "applib/app.h"
#include "applib/app_timer.h"
#include "applib/event_service_client.h"
#include "applib/fonts/fonts.h"
#include "applib/graphics/gpath.h"
#include "applib/graphics/graphics.h"
#include "applib/ui/ui.h"
#include "apps/system/common/app_text_util.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "pbl/services/ecompass.h"
#include "pbl/services/i18n/i18n.h"
#include "pbl/util/math.h"
#include "pbl/util/trig.h"

#include <inttypes.h>
#include <stdio.h>

// The dial glides to a new reading instead of snapping to it: readings arrive in
// steps of a couple of degrees, and a dial that jumps between them is hard to
// read while walking. Each frame closes a fixed fraction of what is left, which
// is fast when the wrist turns and gentle when it settles.
#define GLIDE_FRAME_MS    33
#define GLIDE_NUMERATOR   3
#define GLIDE_DENOMINATOR 10
//! Below a third of a degree the movement is under one pixel at the rim, so the
//! frame timer stops rather than redrawing the screen forever.
#define GLIDE_SETTLED (TRIG_MAX_ANGLE / 1080)

#define DIAL_MARGIN       10
#define TICK_LEN          7
#define MAJOR_TICK_LEN    11
#define LABEL_INSET       24
#define NEEDLE_HALF_WIDTH 9

//! Bands from the bottom up: the readout, a status line that is empty once the
//! compass is calibrated, and whatever is left goes to the dial.
#define READOUT_H 40
#define CAPTION_H 18

// The letters ride the dial, so each one is drawn in its own box centred on the
// point it belongs to rather than laid out as a line of text.
#define LABEL_BOX_W 22
#define LABEL_BOX_H 22

typedef struct CompassAppData {
  Window window;
  Layer dial_layer;

  //! Latest reading, as the compass event delivers it.
  CompassHeading magnetic_heading;
  CompassStatus status;
  //! Where the dial is drawn right now; it chases heading.magnetic_heading.
  int32_t shown_heading;
  AppTimer *glide_timer;
  EventServiceInfo compass_event;

  // Cardinal letters and the eight-point name under the dial are translated
  // once at window load: they are redrawn on every heading update.
  char cardinals[4][8];
  char points[8][8];

  char degrees_text[8];
} CompassAppData;

// Bearings of the four dial letters, clockwise from north.
static const int32_t s_cardinal_bearings[4] = {
  0,
  TRIG_MAX_ANGLE / 4,
  TRIG_MAX_ANGLE / 2,
  (TRIG_MAX_ANGLE * 3) / 4,
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing helpers

//! Point at `angle` (clockwise from straight up) `radius` away from `center`.
static GPoint prv_polar(GPoint center, int32_t angle, int32_t radius) {
  return GPoint(center.x + (int16_t)((sin_lookup(angle) * radius) / TRIG_MAX_RATIO),
                center.y - (int16_t)((cos_lookup(angle) * radius) / TRIG_MAX_RATIO));
}

//! Where a bearing sits on screen. The service reports a heading that grows
//! counter-clockwise from north, which is exactly the offset the dial has to be
//! turned by: facing east puts north on the left.
static int32_t prv_screen_angle(const CompassAppData *data, int32_t bearing) {
  return bearing + data->shown_heading;
}

static void prv_draw_ticks(GContext *ctx, const CompassAppData *data, GPoint center,
                           int32_t radius) {
  graphics_context_set_stroke_color(ctx, GColorBlack);
  for (int i = 0; i < 12; i++) {
    const int32_t bearing = (TRIG_MAX_ANGLE * i) / 12;
    const bool major = (i % 3) == 0;
    const int32_t len = major ? MAJOR_TICK_LEN : TICK_LEN;
    graphics_context_set_stroke_width(ctx, major ? 3 : 1);
    const int32_t angle = prv_screen_angle(data, bearing);
    graphics_draw_line(ctx, prv_polar(center, angle, radius),
                       prv_polar(center, angle, radius - len));
  }
  graphics_context_set_stroke_width(ctx, 1);
}

static void prv_draw_cardinals(GContext *ctx, const CompassAppData *data, GPoint center,
                               int32_t radius) {
  GFont const font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  for (int i = 0; i < 4; i++) {
    const int32_t angle = prv_screen_angle(data, s_cardinal_bearings[i]);
    const GPoint p = prv_polar(center, angle, radius - LABEL_INSET);
    // North is the only letter that carries the colour; the rest stay black so
    // the one that matters is findable at a glance on the wrist.
    graphics_context_set_text_color(ctx, (i == 0) ? GColorRed : GColorBlack);
    const GRect box = GRect(p.x - LABEL_BOX_W / 2, p.y - LABEL_BOX_H / 2, LABEL_BOX_W, LABEL_BOX_H);
    graphics_draw_text(ctx, data->cardinals[i], font, box, GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
  }
}

//! The needle is fixed: it always points up, at whatever the wearer is facing.
//! The red half marks north, so it is the dial that turns underneath it.
static void prv_draw_needle(GContext *ctx, GPoint center, int32_t radius) {
  const int32_t len = radius - MAJOR_TICK_LEN - 6;
  const GPathInfo north = {
    .num_points = 3,
    .points = (GPoint[]){
      {center.x, (int16_t)(center.y - len)},
      {(int16_t)(center.x - NEEDLE_HALF_WIDTH), center.y},
      {(int16_t)(center.x + NEEDLE_HALF_WIDTH), center.y},
    },
  };
  const GPathInfo south = {
    .num_points = 3,
    .points = (GPoint[]){
      {center.x, (int16_t)(center.y + len)},
      {(int16_t)(center.x - NEEDLE_HALF_WIDTH), center.y},
      {(int16_t)(center.x + NEEDLE_HALF_WIDTH), center.y},
    },
  };

  GPath path;

  gpath_init(&path, &north);
  graphics_context_set_fill_color(ctx, GColorRed);
  gpath_draw_filled(ctx, &path);

  gpath_init(&path, &south);
  graphics_context_set_fill_color(ctx, GColorWhite);
  gpath_draw_filled(ctx, &path);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  gpath_draw_outline(ctx, &path);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, center, 4);
}

//! Degrees clockwise from north, which is what a heading means to the wearer.
static int32_t prv_bearing_degrees(const CompassAppData *data) {
  const int32_t clockwise =
      (TRIG_MAX_ANGLE - (data->magnetic_heading % TRIG_MAX_ANGLE)) % TRIG_MAX_ANGLE;
  return TRIGANGLE_TO_DEG(clockwise) % 360;
}

static void prv_draw_readout(GContext *ctx, CompassAppData *data, GRect bounds) {
  const int32_t degrees = prv_bearing_degrees(data);
  // Round to the nearest of the eight points: each spans 45 degrees.
  const int point = ((degrees + 22) / 45) % 8;

  snprintf(data->degrees_text, sizeof(data->degrees_text), "%" PRId32 "°", degrees);

  GFont const font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  const int16_t top = bounds.size.h - READOUT_H;
  const int16_t half = bounds.size.w / 2;

  graphics_context_set_text_color(ctx, GColorBlack);
  app_draw_text_centred(ctx, data->degrees_text, font, GRect(0, top, half, READOUT_H));
  app_draw_text_centred(ctx, data->points[point], font, GRect(half, top, half, READOUT_H));

  // While calibration refines, the numbers already mean something. The status
  // line keeps its space either way so the dial does not jump when it clears.
  if (data->status == CompassStatusCalibrating) {
    graphics_context_set_text_color(ctx, GColorOrange);
    app_draw_text_centred(ctx, i18n_get(i18n_noop("Calibrating"), data),
                          fonts_get_system_font(FONT_KEY_GOTHIC_18),
                          GRect(0, top - CAPTION_H, bounds.size.w, CAPTION_H));
  }
}

//! Until the service has a fix the numbers would be a lie, so the dial gives way
//! to the instruction that produces one — or, if there is no magnetometer at
//! all, to the plain statement that there is nothing to show.
static void prv_draw_no_heading(GContext *ctx, GRect bounds, bool unavailable) {
  char message[64];
  i18n_get_with_buffer(
      unavailable ? i18n_noop("No compass") : i18n_noop("Wave your arm in a figure eight"), message,
      sizeof(message));

  graphics_context_set_text_color(ctx, GColorBlack);
  const GRect box = grect_inset(bounds, GEdgeInsets(bounds.size.h / 3, 12, 0, 12));
  graphics_draw_text(ctx, message, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), box,
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void prv_dial_update_proc(Layer *layer, GContext *ctx) {
  CompassAppData *data = window_get_user_data(layer_get_window(layer));
  const GRect bounds = layer->bounds;

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, &bounds);

  if (data->status == CompassStatusDataInvalid || data->status == CompassStatusUnavailable) {
    prv_draw_no_heading(ctx, bounds, data->status == CompassStatusUnavailable);
    return;
  }

  const int16_t dial_h = bounds.size.h - READOUT_H - CAPTION_H;
  const GPoint center = GPoint(bounds.size.w / 2, dial_h / 2);
  const int32_t radius = MIN(bounds.size.w, dial_h) / 2 - DIAL_MARGIN;

  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_circle(ctx, center, radius);

  prv_draw_ticks(ctx, data, center, radius);
  prv_draw_cardinals(ctx, data, center, radius);
  prv_draw_needle(ctx, center, radius);
  prv_draw_readout(ctx, data, bounds);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Gliding to the new reading

//! Signed angle from `from` to `to`, taking the short way round the circle.
static int32_t prv_shortest_delta(int32_t from, int32_t to) {
  int32_t delta = (to - from) % TRIG_MAX_ANGLE;
  if (delta > TRIG_MAX_ANGLE / 2) {
    delta -= TRIG_MAX_ANGLE;
  } else if (delta < -TRIG_MAX_ANGLE / 2) {
    delta += TRIG_MAX_ANGLE;
  }
  return delta;
}

static void prv_glide_frame(void *context) {
  CompassAppData *data = context;
  data->glide_timer = NULL;

  const int32_t delta = prv_shortest_delta(data->shown_heading, data->magnetic_heading);
  if (ABS(delta) <= GLIDE_SETTLED) {
    data->shown_heading = data->magnetic_heading;
    layer_mark_dirty(&data->dial_layer);
    return;
  }

  int32_t step = (delta * GLIDE_NUMERATOR) / GLIDE_DENOMINATOR;
  if (step == 0) {
    // Integer division would stall the dial a hair short of the reading.
    step = (delta > 0) ? 1 : -1;
  }
  data->shown_heading = (data->shown_heading + step + TRIG_MAX_ANGLE) % TRIG_MAX_ANGLE;

  layer_mark_dirty(&data->dial_layer);
  data->glide_timer = app_timer_register(GLIDE_FRAME_MS, prv_glide_frame, data);
}

static void prv_glide_start(CompassAppData *data) {
  if (!data->glide_timer) {
    data->glide_timer = app_timer_register(GLIDE_FRAME_MS, prv_glide_frame, data);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Window

//! Subscribed straight to the compass event rather than through
//! applib's compass service: the service is only built where there is a
//! magnetometer driver, while the event is also what the emulator posts, so
//! taking it directly is what makes the screen testable off the wrist.
static void prv_compass_event_handler(PebbleEvent *event, void *context) {
  CompassAppData *data = context;
  const PebbleCompassDataEvent *reading = &event->compass_data;
  const bool had_heading =
      (data->status != CompassStatusDataInvalid && data->status != CompassStatusUnavailable);

  data->magnetic_heading = reading->magnetic_heading;
  data->status = reading->calib_status;

  if (!had_heading) {
    // Nothing to glide from on the first reading: start where it points.
    data->shown_heading = data->magnetic_heading;
    layer_mark_dirty(&data->dial_layer);
    return;
  }
  prv_glide_start(data);
}

static void prv_load_labels(CompassAppData *data) {
  // Single letters collide with everything, so they carry a context that says
  // what they are: the translator sees "Compass|N", not a bare "N".
  static const char *const cardinals[4] = {
    i18n_ctx_noop("Compass", "N"),
    i18n_ctx_noop("Compass", "E"),
    i18n_ctx_noop("Compass", "S"),
    i18n_ctx_noop("Compass", "W"),
  };
  static const char *const points[8] = {
    i18n_ctx_noop("Compass", "N"),  i18n_ctx_noop("Compass", "NE"), i18n_ctx_noop("Compass", "E"),
    i18n_ctx_noop("Compass", "SE"), i18n_ctx_noop("Compass", "S"),  i18n_ctx_noop("Compass", "SW"),
    i18n_ctx_noop("Compass", "W"),  i18n_ctx_noop("Compass", "NW"),
  };

  for (int i = 0; i < 4; i++) {
    i18n_get_with_buffer(cardinals[i], data->cardinals[i], sizeof(data->cardinals[i]));
  }
  for (int i = 0; i < 8; i++) {
    i18n_get_with_buffer(points[i], data->points[i], sizeof(data->points[i]));
  }
}

static void prv_window_load(Window *window) {
  CompassAppData *data = window_get_user_data(window);

  prv_load_labels(data);

  Layer *root = window_get_root_layer(window);
  layer_init(&data->dial_layer, &root->bounds);
  layer_set_update_proc(&data->dial_layer, prv_dial_update_proc);
  layer_add_child(root, &data->dial_layer);

  data->compass_event = (EventServiceInfo){
    .type = PEBBLE_COMPASS_DATA_EVENT,
    .handler = prv_compass_event_handler,
    .context = data,
  };
  event_service_client_subscribe(&data->compass_event);
}

static void prv_window_unload(Window *window) {
  CompassAppData *data = window_get_user_data(window);
  event_service_client_unsubscribe(&data->compass_event);
  if (data->glide_timer) {
    app_timer_cancel(data->glide_timer);
    data->glide_timer = NULL;
  }
  i18n_free_all(data);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// App main

static void prv_main(void) {
  CompassAppData *data = app_zalloc_check(sizeof(CompassAppData));
  app_state_set_user_data(data);

  // Nothing is known until the first event; treat that as "not calibrated yet"
  // so the hint, and not a stale north, is what greets the wearer.
  data->status = CompassStatusDataInvalid;

  window_init(&data->window, WINDOW_NAME("Compass"));
  window_set_user_data(&data->window, data);
  window_set_window_handlers(&data->window, &(WindowHandlers){
                                              .load = prv_window_load,
                                              .unload = prv_window_unload,
                                            });
  app_window_stack_push(&data->window, true /* animated */);

  app_event_loop();

  app_free(data);
}

const PebbleProcessMd *compass_app_get_info(void) {
  static const PebbleProcessMdSystem s_compass_app_info = {
    .common =
        {
          .main_func = &prv_main,
          .uuid = COMPASS_APP_UUID,
        },
    .name = i18n_noop("Compass"),
    .icon_resource_id = RESOURCE_ID_COMPASS_TINY,
  };
  return (const PebbleProcessMd *)&s_compass_app_info;
}

#endif // CONFIG_APP_COMPASS
