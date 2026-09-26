/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "flashlight.h"

#include "applib/app.h"
#include "applib/app_timer.h"
#include "applib/fonts/fonts.h"
#include "applib/graphics/graphics.h"
#include "applib/ui/ui.h"
#include <pbl/drivers/backlight.h>
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "pbl/services/i18n/i18n.h"
#include "pbl/services/light.h"
#include "pbl/util/math.h"
#include "pbl/util/size.h"

#include <inttypes.h>
#include <stdio.h>

// The screen is reflective, so most of the light the wearer gets comes off a
// white field lit by the frontlight. Anything drawn on it is light taken away,
// which is why the hint disappears once the wearer has had a moment to read it.
#define HINT_TIMEOUT_MS 3000
#define HINT_H          30

// Brightness ramps rather than stepping. A torch that jumps between levels
// reads as a fault; a quarter-second ramp reads as a dimmer being turned.
#define RAMP_FRAME_MS 25
#define RAMP_STEPS    10

static const uint8_t s_levels[] = {100, 60, 30, 10};

typedef struct FlashlightAppData {
  Window window;
  Layer light_layer;

  AppTimer *hint_timer;
  bool hint_visible;

  uint8_t level_idx;
  bool night_mode;
  //! Level to come back to when night mode is switched off.
  uint8_t day_level_idx;

  //! Brightness actually being driven, ramping towards s_levels[level_idx].
  uint8_t shown_level;
  AppTimer *ramp_timer;

  char hint[64];
} FlashlightAppData;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Light

static void prv_ramp_frame(void *context) {
  FlashlightAppData *data = context;
  data->ramp_timer = NULL;

  const uint8_t target = s_levels[data->level_idx];
  const int16_t delta = (int16_t)target - (int16_t)data->shown_level;
  if (delta == 0) {
    return;
  }

  int16_t step = (int16_t)(100 / RAMP_STEPS);
  if (delta < 0) {
    step = -step;
  }
  if (ABS(delta) <= ABS(step)) {
    data->shown_level = target;
  } else {
    data->shown_level = (uint8_t)(data->shown_level + step);
    data->ramp_timer = app_timer_register(RAMP_FRAME_MS, prv_ramp_frame, data);
  }

  light_set_intensity_override(data->shown_level);
}

static void prv_apply(FlashlightAppData *data) {
  // The backlight keeps the colour the wearer set for the watch. Night mode is
  // the black screen and the lowest level, not a tinted backlight: a torch that
  // repaints the backlight leaves the wearer wondering what else changed it.
  light_enable(true);
  if (!data->ramp_timer) {
    data->ramp_timer = app_timer_register(RAMP_FRAME_MS, prv_ramp_frame, data);
  }
  layer_mark_dirty(&data->light_layer);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing

static void prv_light_update_proc(Layer *layer, GContext *ctx) {
  FlashlightAppData *data = window_get_user_data(layer_get_window(layer));
  const GRect bounds = layer->bounds;

  // In night mode the field stays dark: a red frontlight over white paper is
  // still a bright patch, and the point of the mode is not to be one.
  const GColor field = data->night_mode ? GColorBlack : GColorWhite;
  graphics_context_set_fill_color(ctx, field);
  graphics_fill_rect(ctx, &bounds);

  // In night mode the readout stays: text on a black field costs no light, and
  // an all-black screen with nothing on it looks like a watch that has died.
  if (!data->hint_visible && !data->night_mode) {
    return;
  }

  snprintf(data->hint, sizeof(data->hint), "%" PRIu8 "%%", s_levels[data->level_idx]);
  graphics_context_set_text_color(ctx, gcolor_legible_over(field));
  const GRect box = GRect(0, bounds.size.h - HINT_H, bounds.size.w, HINT_H);
  graphics_draw_text(ctx, data->hint, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), box,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Buttons

static void prv_hint_timeout(void *context) {
  FlashlightAppData *data = context;
  data->hint_timer = NULL;
  data->hint_visible = false;
  layer_mark_dirty(&data->light_layer);
}

static void prv_show_hint(FlashlightAppData *data) {
  data->hint_visible = true;
  if (data->hint_timer) {
    app_timer_cancel(data->hint_timer);
  }
  data->hint_timer = app_timer_register(HINT_TIMEOUT_MS, prv_hint_timeout, data);
}

static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  FlashlightAppData *data = app_state_get_user_data();
  if (data->level_idx > 0) {
    data->level_idx--;
  }
  prv_show_hint(data);
  prv_apply(data);
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  FlashlightAppData *data = app_state_get_user_data();
  if (data->level_idx + 1u < ARRAY_LENGTH(s_levels)) {
    data->level_idx++;
  }
  prv_show_hint(data);
  prv_apply(data);
}

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  FlashlightAppData *data = app_state_get_user_data();
  data->night_mode = !data->night_mode;
  // Night mode is the black field and the lowest level, as prv_apply says. It
  // used to keep whatever level was set, so a torch at 100 % stayed at 100 %
  // behind a black screen. The day level comes back when night mode goes off.
  if (data->night_mode) {
    data->day_level_idx = data->level_idx;
    data->level_idx = ARRAY_LENGTH(s_levels) - 1;
  } else {
    data->level_idx = data->day_level_idx;
  }
  prv_show_hint(data);
  prv_apply(data);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Window

static void prv_window_load(Window *window) {
  FlashlightAppData *data = window_get_user_data(window);

  Layer *root = window_get_root_layer(window);
  layer_init(&data->light_layer, &root->bounds);
  layer_set_update_proc(&data->light_layer, prv_light_update_proc);
  layer_add_child(root, &data->light_layer);

  // The first level is not ramped to from nothing: the wearer opened a torch and
  // wants light now.
  data->shown_level = s_levels[data->level_idx];
  light_set_intensity_override(data->shown_level);

  prv_show_hint(data);
  prv_apply(data);
}

//! Окно снова на экране после того, что его перекрывало. Будильник, закрываясь,
//! выключает подсветку (alarm_popup.c, light_enable(false)), и фонарик
//! оставался белым экраном без света до первой кнопки.
static void prv_window_appear(Window *window) {
  prv_apply(window_get_user_data(window));
}

static void prv_window_unload(Window *window) {
  FlashlightAppData *data = window_get_user_data(window);
  if (data->hint_timer) {
    app_timer_cancel(data->hint_timer);
    data->hint_timer = NULL;
  }
  if (data->ramp_timer) {
    app_timer_cancel(data->ramp_timer);
    data->ramp_timer = NULL;
  }
  // app_manager clears the override on exit, but the app may outlive this
  // window; leave the light as we found it either way.
  light_set_intensity_override(0);
  light_enable(false);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// App main

static void prv_main(void) {
  FlashlightAppData *data = app_zalloc_check(sizeof(FlashlightAppData));
  app_state_set_user_data(data);

  window_init(&data->window, WINDOW_NAME("Flashlight"));
  window_set_user_data(&data->window, data);
  window_set_click_config_provider(&data->window, prv_click_config_provider);
  window_set_window_handlers(&data->window, &(WindowHandlers){
                                              .load = prv_window_load,
                                              .appear = prv_window_appear,
                                              .unload = prv_window_unload,
                                            });
  app_window_stack_push(&data->window, true /* animated */);

  app_event_loop();

  app_free(data);
}

const PebbleProcessMd *flashlight_app_get_info(void) {
  static const PebbleProcessMdSystem s_flashlight_app_info = {
    .common =
        {
          .main_func = &prv_main,
          .uuid = FLASHLIGHT_APP_UUID,
        },
    .name = i18n_noop("Flashlight"),
    .icon_resource_id = RESOURCE_ID_FLASHLIGHT_TINY,
  };
  return (const PebbleProcessMd *)&s_flashlight_app_info;
}
