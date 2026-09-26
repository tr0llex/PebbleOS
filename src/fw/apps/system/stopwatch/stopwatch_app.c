/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "stopwatch_app.h"

// Сервис может быть выключен в конфигурации, а CMake собирает все файлы
// каталога: без этой обёртки сборка без сервиса падала бы на ссылках.
#ifdef CONFIG_SERVICE_STOPWATCH

#include "applib/app.h"
#include "applib/app_timer.h"
#include "applib/fonts/fonts.h"
#include "applib/graphics/graphics.h"
#include "applib/ui/action_bar_layer.h"
#include "applib/ui/animation.h"
#include "applib/ui/ui.h"
#include "apps/system/common/app_text_util.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "pbl/services/i18n/i18n.h"
#include "pbl/services/stopwatch/stopwatch.h"
#include "pbl/util/size.h"
#include "shell/prefs.h"

#include <inttypes.h>
#include <stdio.h>

// While running, the readout is redrawn ten times a second so the tenths move;
// paused, nothing changes and no timer runs.
#define STOPWATCH_TICK_MS 100
#define TIMER_TICK_MS     200

// Bands measured off screenshots: LECO 42 digits ink 29 px tall, a GOTHIC 18
// row 11 px on a 22 px pitch. The stopwatch keeps its digits in the upper third
// so the lap list does not shove them when the first lap lands; the countdown
// has no list, so its whole group is centred.
#define TIME_BAND_TOP 40
#define TIME_BAND_H   56
#define LAP_LIST_TOP  104
#define LAP_ROW_H     22
#define LAP_LEFT      10

#define TIMER_GROUP_TOP   49
#define TIMER_CAPTION_TOP 113
#define TIMER_CAPTION_H   26
#define TIMER_BAR_TOP     149
#define TIMER_BAR_H       14
#define TIMER_BAR_MARGIN  14

// A new lap pushes the list down rather than appearing on top of it, so it is
// obvious which row is the one that was just taken.
#define LAP_SLIDE_MS 220
// When the countdown reaches zero the screen inverts a few times: the alert
// vibrates, and the wearer who looks down a moment later should still see it.
#define EXPIRED_FLASH_MS 1800
#define EXPIRED_FLASHES  3

#define TIME_TEXT_LEN 16

//! Offered countdowns, in minutes. Anything longer is better served by an alarm.
static const uint16_t s_timer_presets_m[] = {1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 90};

typedef enum RootRow {
  RootRowStopwatch = 0,
  RootRowTimer,
  RootRowCount,
} RootRow;

typedef struct StopwatchAppData {
  Animation *lap_slide;
  //! Pixels the lap list is still shifted up by while a new lap slides in.
  int16_t lap_offset;

  Animation *expired_flash;
  bool inverted;
  //! Отсчёт шёл при открытом приложении. Мигаем только тогда: таймер, истёкший
  //! час назад, при каждом открытии мигал заново.
  bool timer_seen_running;

  Window root_window;
  MenuLayer root_menu;

  Window stopwatch_window;
  Layer stopwatch_layer;
  ActionBarLayer stopwatch_action_bar;
  AppTimer *stopwatch_tick;

  Window presets_window;
  MenuLayer presets_menu;

  Window timer_window;
  Layer timer_layer;
  ActionBarLayer timer_action_bar;
  AppTimer *timer_tick;

  GBitmap *icon_start;
  GBitmap *icon_pause;
  GBitmap *icon_stop;
  GBitmap *icon_more;

  char time_text[TIME_TEXT_LEN];
  char lap_text[TIME_TEXT_LEN];
  char row_text[32];
} StopwatchAppData;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Formatting

//! "M:SS.t" while under an hour, "H:MM:SS" past it. The tenth is what makes a
//! stopwatch feel like one, so it is dropped only when the hours need the room.
static void prv_format_ms(char *buf, size_t len, uint32_t ms, bool tenths) {
  const uint32_t total_s = ms / 1000;
  const uint32_t h = total_s / 3600;
  const uint32_t m = (total_s / 60) % 60;
  const uint32_t s = total_s % 60;

  if (h > 0) {
    snprintf(buf, len, "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, h, m, s);
  } else if (tenths) {
    snprintf(buf, len, "%" PRIu32 ":%02" PRIu32 ".%" PRIu32, m, s, (ms % 1000) / 100);
  } else {
    snprintf(buf, len, "%" PRIu32 ":%02" PRIu32, m, s);
  }
}

static void prv_draw_big_time(GContext *ctx, GRect bounds, const char *text, int16_t band_top,
                              GColor ink) {
  graphics_context_set_text_color(ctx, ink);
  app_draw_text_centred(ctx, text, fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS),
                        GRect(0, band_top, bounds.size.w, TIME_BAND_H));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Stopwatch window

static void prv_stopwatch_update_proc(Layer *layer, GContext *ctx) {
  StopwatchAppData *data = window_get_user_data(layer_get_window(layer));
  const GRect bounds = layer->bounds;

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, &bounds);

  prv_format_ms(data->time_text, sizeof(data->time_text), stopwatch_get_elapsed_ms(), true);
  prv_draw_big_time(ctx, bounds, data->time_text, TIME_BAND_TOP, GColorBlack);

  const uint8_t laps = stopwatch_get_lap_count();
  const uint16_t lap_total = stopwatch_get_lap_total();
  const int16_t list_top = LAP_LIST_TOP;
  const int max_rows = (bounds.size.h - list_top) / LAP_ROW_H;

  graphics_context_set_text_color(ctx, GColorBlack);
  GFont const font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  for (int i = 0; i < max_rows && i < laps; i++) {
    prv_format_ms(data->lap_text, sizeof(data->lap_text), stopwatch_get_lap_ms(i), true);
    // Number the laps as they were taken, so the newest keeps the highest number
    // instead of every row renumbering when a lap is added.
    // The kept count stops at STOPWATCH_MAX_LAPS; the total does not.
    snprintf(data->row_text, sizeof(data->row_text), "%d   %s", lap_total - i, data->lap_text);
    const GRect box = GRect(LAP_LEFT, list_top + i * LAP_ROW_H + data->lap_offset,
                            bounds.size.w - LAP_LEFT, LAP_ROW_H);
    graphics_draw_text(ctx, data->row_text, font, box, GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
  }
}

static void prv_stopwatch_sync(StopwatchAppData *data);

//! The list starts a row high and settles into place, so the row that appeared
//! is the one the eye follows.
static void prv_lap_slide_update(Animation *animation, const AnimationProgress progress) {
  StopwatchAppData *data = animation_get_context(animation);
  data->lap_offset =
      (int16_t)((-LAP_ROW_H * (ANIMATION_NORMALIZED_MAX - progress)) / ANIMATION_NORMALIZED_MAX);
  layer_mark_dirty(&data->stopwatch_layer);
}

static const AnimationImplementation s_lap_slide_impl = {
  .update = prv_lap_slide_update,
};

//! Отыгравшая анимация уничтожается сама; указатель обнуляем, иначе следующее
//! animation_destroy пишет в журнал «Animation does not exist».
static void prv_lap_slide_stopped(Animation *animation, bool finished, void *context) {
  StopwatchAppData *data = context;
  if (data->lap_slide == animation) {
    data->lap_slide = NULL;
  }
}

static void prv_lap_slide_start(StopwatchAppData *data) {
  if (data->lap_slide) {
    // Живая анимация уничтожается при снятии сама (auto_destroy), а обработчик
    // stopped обнуляет указатель; отдельный animation_destroy был бы вторым.
    animation_unschedule(data->lap_slide);
    data->lap_slide = NULL;
  }
  data->lap_slide = animation_create();
  if (!data->lap_slide) {
    data->lap_offset = 0;
    return;
  }
  animation_set_implementation(data->lap_slide, &s_lap_slide_impl);
  animation_set_handlers(data->lap_slide, (AnimationHandlers){.stopped = prv_lap_slide_stopped},
                         data);
  animation_set_duration(data->lap_slide, LAP_SLIDE_MS);
  animation_set_curve(data->lap_slide, AnimationCurveEaseOut);
  animation_schedule(data->lap_slide);
}

static void prv_stopwatch_tick_cb(void *context) {
  StopwatchAppData *data = context;
  data->stopwatch_tick = NULL;
  layer_mark_dirty(&data->stopwatch_layer);
  if (stopwatch_is_running()) {
    data->stopwatch_tick = app_timer_register(STOPWATCH_TICK_MS, prv_stopwatch_tick_cb, data);
  }
}

//! Bring the action bar and the redraw timer in line with the service state.
static void prv_stopwatch_sync(StopwatchAppData *data) {
  const bool running = stopwatch_is_running();
  ActionBarLayer *bar = &data->stopwatch_action_bar;

  action_bar_layer_set_icon(bar, BUTTON_ID_SELECT, running ? data->icon_pause : data->icon_start);
  if (running) {
    action_bar_layer_set_icon(bar, BUTTON_ID_UP, data->icon_more);
    action_bar_layer_clear_icon(bar, BUTTON_ID_DOWN);
  } else {
    action_bar_layer_clear_icon(bar, BUTTON_ID_UP);
    // Reset is offered only while paused: it is the one action that throws away
    // what the wearer has been measuring.
    action_bar_layer_set_icon(bar, BUTTON_ID_DOWN,
                              stopwatch_get_elapsed_ms() > 0 ? data->icon_stop : NULL);
  }

  if (running && !data->stopwatch_tick) {
    data->stopwatch_tick = app_timer_register(STOPWATCH_TICK_MS, prv_stopwatch_tick_cb, data);
  } else if (!running && data->stopwatch_tick) {
    app_timer_cancel(data->stopwatch_tick);
    data->stopwatch_tick = NULL;
  }

  layer_mark_dirty(&data->stopwatch_layer);
}

static void prv_stopwatch_select_click(ClickRecognizerRef recognizer, void *context) {
  StopwatchAppData *data = context;
  if (stopwatch_is_running()) {
    stopwatch_pause();
  } else {
    stopwatch_start();
  }
  prv_stopwatch_sync(data);
}

static void prv_stopwatch_up_click(ClickRecognizerRef recognizer, void *context) {
  StopwatchAppData *data = context;
  const uint8_t before = stopwatch_get_lap_count();
  stopwatch_lap();
  if (stopwatch_get_lap_count() != before || stopwatch_is_running()) {
    prv_lap_slide_start(data);
  }
  prv_stopwatch_sync(data);
}

static void prv_stopwatch_down_click(ClickRecognizerRef recognizer, void *context) {
  StopwatchAppData *data = context;
  if (!stopwatch_is_running()) {
    stopwatch_reset();
  }
  prv_stopwatch_sync(data);
}

static void prv_stopwatch_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_stopwatch_select_click);
  window_single_click_subscribe(BUTTON_ID_UP, prv_stopwatch_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_stopwatch_down_click);
}

static void prv_stopwatch_window_load(Window *window) {
  StopwatchAppData *data = window_get_user_data(window);

  GRect bounds = window->layer.bounds;
  bounds.size.w -= ACTION_BAR_WIDTH;
  layer_init(&data->stopwatch_layer, &bounds);
  layer_set_update_proc(&data->stopwatch_layer, prv_stopwatch_update_proc);
  layer_add_child(&window->layer, &data->stopwatch_layer);

  ActionBarLayer *bar = &data->stopwatch_action_bar;
  action_bar_layer_init(bar);
  action_bar_layer_set_context(bar, data);
  action_bar_layer_set_click_config_provider(bar, prv_stopwatch_click_config_provider);
  action_bar_layer_add_to_window(bar, window);

  prv_stopwatch_sync(data);
}

static void prv_stopwatch_window_unload(Window *window) {
  StopwatchAppData *data = window_get_user_data(window);
  if (data->stopwatch_tick) {
    app_timer_cancel(data->stopwatch_tick);
    data->stopwatch_tick = NULL;
  }
  if (data->lap_slide) {
    // Живая анимация уничтожается при снятии сама (auto_destroy), а обработчик
    // stopped обнуляет указатель; отдельный animation_destroy был бы вторым.
    animation_unschedule(data->lap_slide);
    data->lap_slide = NULL;
  }
  action_bar_layer_deinit(&data->stopwatch_action_bar);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Countdown window

//! Формат из языкового пака идёт прямо в snprintf. Чужой пак с %d или двумя %s
//! прочитал бы лишние аргументы в привилегированном приложении, поэтому формат
//! принимается, только если в нём ровно один %s и других подстановок нет.
static const char *prv_one_string_format(const char *format) {
  int specs = 0;
  for (const char *p = format; *p; p++) {
    if (*p != '%') {
      continue;
    }
    if (p[1] == '%') {
      p++;
      continue;
    }
    if (p[1] != 's') {
      return "%s";
    }
    specs++;
  }
  return (specs == 1) ? format : "%s";
}

static void prv_timer_update_proc(Layer *layer, GContext *ctx) {
  StopwatchAppData *data = window_get_user_data(layer_get_window(layer));
  const GRect bounds = layer->bounds;

  const GColor field = data->inverted ? GColorBlack : GColorWhite;
  const GColor ink = data->inverted ? GColorWhite : GColorBlack;
  graphics_context_set_fill_color(ctx, field);
  graphics_fill_rect(ctx, &bounds);

  const uint32_t remaining_ms = stopwatch_timer_get_remaining_ms();
  const uint32_t duration_ms = stopwatch_timer_get_duration_s() * 1000;
  // Round up: a countdown showing 0:00 that has not gone off reads as broken.
  prv_format_ms(data->time_text, sizeof(data->time_text), (remaining_ms + 999) / 1000 * 1000,
                false);
  prv_draw_big_time(ctx, bounds, data->time_text, TIMER_GROUP_TOP, ink);

  // The line under the digits is never empty: while it counts down it names the
  // timer that is running. It used to read "of 1:00", which on the first second
  // repeated the digits above it and, in Russian, left «из» hanging on its own.
  const char *state;
  if (remaining_ms == 0) {
    state = i18n_get(i18n_noop("Time is up"), data);
  } else if (!stopwatch_timer_is_running()) {
    state = i18n_get(i18n_noop("Paused"), data);
  } else {
    // Presets are whole minutes, so minutes are what the wearer picked.
    snprintf(data->lap_text, sizeof(data->lap_text), "%" PRIu32 " %s", duration_ms / 60000,
             i18n_get(i18n_ctx_noop("Duration", "min"), data));
    snprintf(data->row_text, sizeof(data->row_text),
             prv_one_string_format(i18n_get(i18n_noop("Timer for %s"), data)), data->lap_text);
    state = data->row_text;
  }
  graphics_context_set_text_color(ctx, ink);
  app_draw_text_centred(ctx, state, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                        GRect(0, TIMER_CAPTION_TOP, bounds.size.w, TIMER_CAPTION_H));

  // The progress bar turns the remaining seconds into something readable at a
  // glance, which is the whole point of looking at a countdown.
  if (duration_ms > 0) {
    const GRect track =
        GRect(TIMER_BAR_MARGIN, TIMER_BAR_TOP, bounds.size.w - 2 * TIMER_BAR_MARGIN, TIMER_BAR_H);
    graphics_context_set_stroke_color(ctx, ink);
    graphics_draw_rect(ctx, &track);
    GRect fill = grect_inset(track, GEdgeInsets(2));
    fill.size.w = (int16_t)((int32_t)fill.size.w * remaining_ms / duration_ms);
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_rect(ctx, &fill);
  }
}

static void prv_timer_sync(StopwatchAppData *data);
static void prv_expired_flash_start(StopwatchAppData *data);

static void prv_timer_tick_cb(void *context) {
  StopwatchAppData *data = context;
  data->timer_tick = NULL;
  if (stopwatch_timer_is_running()) {
    data->timer_tick = app_timer_register(TIMER_TICK_MS, prv_timer_tick_cb, data);
    layer_mark_dirty(&data->timer_layer);
  } else {
    // The service stopped it: that is the countdown expiring, so pick up the
    // finished state rather than only redrawing.
    prv_timer_sync(data);
  }
}

static void prv_timer_sync(StopwatchAppData *data) {
  const bool running = stopwatch_timer_is_running();
  const bool finished = stopwatch_timer_get_remaining_ms() == 0;
  ActionBarLayer *bar = &data->timer_action_bar;

  if (running) {
    data->timer_seen_running = true;
  }
  if (finished) {
    if (data->timer_seen_running) {
      data->timer_seen_running = false;
      prv_expired_flash_start(data);
    }
    action_bar_layer_clear_icon(bar, BUTTON_ID_SELECT);
  } else {
    action_bar_layer_set_icon(bar, BUTTON_ID_SELECT, running ? data->icon_pause : data->icon_start);
  }
  action_bar_layer_set_icon(bar, BUTTON_ID_DOWN, data->icon_stop);

  if (running && !data->timer_tick) {
    data->timer_tick = app_timer_register(TIMER_TICK_MS, prv_timer_tick_cb, data);
  } else if (!running && data->timer_tick) {
    app_timer_cancel(data->timer_tick);
    data->timer_tick = NULL;
  }

  layer_mark_dirty(&data->timer_layer);
}

static void prv_timer_select_click(ClickRecognizerRef recognizer, void *context) {
  StopwatchAppData *data = context;
  if (stopwatch_timer_get_remaining_ms() == 0) {
    return;
  }
  if (stopwatch_timer_is_running()) {
    stopwatch_timer_pause();
  } else {
    stopwatch_timer_resume();
  }
  prv_timer_sync(data);
}

static void prv_timer_down_click(ClickRecognizerRef recognizer, void *context) {
  StopwatchAppData *data = context;
  stopwatch_timer_cancel();
  window_stack_remove(&data->timer_window, true /* animated */);
}

//! Inverts the screen a few times when the countdown ends. The alert vibrates
//! once; this keeps saying so for a couple of seconds afterwards.
static void prv_expired_flash_update(Animation *animation, const AnimationProgress progress) {
  StopwatchAppData *data = animation_get_context(animation);
  const int32_t phase = ((int32_t)progress * EXPIRED_FLASHES * 2) / ANIMATION_NORMALIZED_MAX;
  const bool inverted = ((phase % 2) == 0);
  if (inverted != data->inverted) {
    data->inverted = inverted;
    layer_mark_dirty(&data->timer_layer);
  }
}

static void prv_expired_flash_stopped(Animation *animation, bool finished, void *context) {
  StopwatchAppData *data = context;
  // Отснявшая анимация уничтожается сама (animation_create ставит
  // auto_destroy), и запомненный указатель становится просто чужим номером.
  // Пока он не обнулён, prv_expired_flash_start считает мигание идущим — и
  // второй отсчёт, заведённый и досчитанный в том же запуске приложения, о
  // своём конце уже не сообщал.
  data->expired_flash = NULL;
  data->inverted = false;
  layer_mark_dirty(&data->timer_layer);
}

static const AnimationImplementation s_expired_flash_impl = {
  .update = prv_expired_flash_update,
};

static void prv_expired_flash_start(StopwatchAppData *data) {
  if (data->expired_flash) {
    return;
  }
  data->expired_flash = animation_create();
  if (!data->expired_flash) {
    return;
  }
  animation_set_implementation(data->expired_flash, &s_expired_flash_impl);
  animation_set_handlers(data->expired_flash,
                         (AnimationHandlers){.stopped = prv_expired_flash_stopped}, data);
  animation_set_duration(data->expired_flash, EXPIRED_FLASH_MS);
  animation_set_curve(data->expired_flash, AnimationCurveLinear);
  animation_schedule(data->expired_flash);
}

static void prv_timer_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_timer_select_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_timer_down_click);
}

static void prv_timer_window_load(Window *window) {
  StopwatchAppData *data = window_get_user_data(window);

  GRect bounds = window->layer.bounds;
  bounds.size.w -= ACTION_BAR_WIDTH;
  layer_init(&data->timer_layer, &bounds);
  layer_set_update_proc(&data->timer_layer, prv_timer_update_proc);
  layer_add_child(&window->layer, &data->timer_layer);

  ActionBarLayer *bar = &data->timer_action_bar;
  action_bar_layer_init(bar);
  action_bar_layer_set_context(bar, data);
  action_bar_layer_set_click_config_provider(bar, prv_timer_click_config_provider);
  action_bar_layer_add_to_window(bar, window);

  prv_timer_sync(data);
}

static void prv_timer_window_unload(Window *window) {
  StopwatchAppData *data = window_get_user_data(window);
  if (data->timer_tick) {
    app_timer_cancel(data->timer_tick);
    data->timer_tick = NULL;
  }
  if (data->expired_flash) {
    // Живая анимация уничтожается при снятии сама (auto_destroy), а обработчик
    // stopped обнуляет указатель; отдельный animation_destroy был бы вторым.
    animation_unschedule(data->expired_flash);
    data->expired_flash = NULL;
  }
  data->inverted = false;
  action_bar_layer_deinit(&data->timer_action_bar);
  i18n_free_all(data);
}

static void prv_push_timer_window(StopwatchAppData *data) {
  window_init(&data->timer_window, WINDOW_NAME("Timer"));
  window_set_user_data(&data->timer_window, data);
  window_set_window_handlers(&data->timer_window, &(WindowHandlers){
                                                    .load = prv_timer_window_load,
                                                    .unload = prv_timer_window_unload,
                                                  });
  app_window_stack_push(&data->timer_window, true /* animated */);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Countdown presets

static uint16_t prv_presets_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  return ARRAY_LENGTH(s_timer_presets_m);
}

static void prv_presets_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                                 void *context) {
  StopwatchAppData *data = context;
  snprintf(data->row_text, sizeof(data->row_text), "%" PRIu16 " %s",
           s_timer_presets_m[cell_index->row], i18n_get(i18n_ctx_noop("Duration", "min"), data));
  menu_cell_basic_draw(ctx, cell_layer, data->row_text, NULL, NULL);
}

static void prv_presets_select(MenuLayer *menu, MenuIndex *cell_index, void *context) {
  StopwatchAppData *data = context;
  stopwatch_timer_start(s_timer_presets_m[cell_index->row] * 60);
  prv_push_timer_window(data);
}

static void prv_presets_window_load(Window *window) {
  StopwatchAppData *data = window_get_user_data(window);

  MenuLayer *menu = &data->presets_menu;
  menu_layer_init(menu, &window->layer.bounds);
  menu_layer_set_callbacks(menu, data,
                           &(MenuLayerCallbacks){
                             .get_num_rows = prv_presets_num_rows,
                             .draw_row = prv_presets_draw_row,
                             .select_click = prv_presets_select,
                           });
  menu_layer_set_highlight_colors(menu, GColorJaegerGreen, GColorWhite);
  menu_layer_set_click_config_onto_window(menu, window);
  menu_layer_set_scroll_wrap_around(menu, shell_prefs_get_menu_scroll_wrap_around_enable());
  layer_add_child(&window->layer, menu_layer_get_layer(menu));
}

static void prv_presets_window_unload(Window *window) {
  StopwatchAppData *data = window_get_user_data(window);
  menu_layer_deinit(&data->presets_menu);
}

static void prv_push_presets_window(StopwatchAppData *data) {
  window_init(&data->presets_window, WINDOW_NAME("Timer Presets"));
  window_set_user_data(&data->presets_window, data);
  window_set_window_handlers(&data->presets_window, &(WindowHandlers){
                                                      .load = prv_presets_window_load,
                                                      .unload = prv_presets_window_unload,
                                                    });
  app_window_stack_push(&data->presets_window, true /* animated */);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Root menu

static uint16_t prv_root_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  return RootRowCount;
}

static void prv_root_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                              void *context) {
  StopwatchAppData *data = context;
  const char *title;
  const char *subtitle = NULL;

  if (cell_index->row == RootRowStopwatch) {
    title = i18n_get(i18n_noop("Stopwatch"), data);
    if (stopwatch_get_elapsed_ms() > 0) {
      prv_format_ms(data->time_text, sizeof(data->time_text), stopwatch_get_elapsed_ms(), false);
      subtitle = data->time_text;
    }
  } else {
    title = i18n_get(i18n_noop("Timer"), data);
    if (stopwatch_timer_is_set()) {
      prv_format_ms(data->lap_text, sizeof(data->lap_text), stopwatch_timer_get_remaining_ms(),
                    false);
      subtitle = data->lap_text;
    }
  }

  menu_cell_basic_draw(ctx, cell_layer, title, subtitle, NULL);
}

static void prv_root_select(MenuLayer *menu, MenuIndex *cell_index, void *context) {
  StopwatchAppData *data = context;
  if (cell_index->row == RootRowStopwatch) {
    window_init(&data->stopwatch_window, WINDOW_NAME("Stopwatch"));
    window_set_user_data(&data->stopwatch_window, data);
    window_set_window_handlers(&data->stopwatch_window, &(WindowHandlers){
                                                          .load = prv_stopwatch_window_load,
                                                          .unload = prv_stopwatch_window_unload,
                                                        });
    app_window_stack_push(&data->stopwatch_window, true /* animated */);
  } else if (stopwatch_timer_is_set()) {
    // A countdown is already running: show it rather than asking for a new one.
    prv_push_timer_window(data);
  } else {
    prv_push_presets_window(data);
  }
}

static void prv_root_window_appear(Window *window) {
  StopwatchAppData *data = window_get_user_data(window);
  menu_layer_reload_data(&data->root_menu);
}

static void prv_root_window_load(Window *window) {
  StopwatchAppData *data = window_get_user_data(window);

  MenuLayer *menu = &data->root_menu;
  menu_layer_init(menu, &window->layer.bounds);
  menu_layer_set_callbacks(menu, data,
                           &(MenuLayerCallbacks){
                             .get_num_rows = prv_root_num_rows,
                             .draw_row = prv_root_draw_row,
                             .select_click = prv_root_select,
                           });
  menu_layer_set_highlight_colors(menu, GColorJaegerGreen, GColorWhite);
  menu_layer_set_click_config_onto_window(menu, window);
  menu_layer_set_scroll_wrap_around(menu, shell_prefs_get_menu_scroll_wrap_around_enable());
  layer_add_child(&window->layer, menu_layer_get_layer(menu));
}

static void prv_root_window_unload(Window *window) {
  StopwatchAppData *data = window_get_user_data(window);
  menu_layer_deinit(&data->root_menu);
  i18n_free_all(data);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// App main

static void prv_main(void) {
  StopwatchAppData *data = app_zalloc_check(sizeof(StopwatchAppData));
  app_state_set_user_data(data);

  data->icon_start = gbitmap_create_with_resource(RESOURCE_ID_ACTION_BAR_ICON_START);
  data->icon_pause = gbitmap_create_with_resource(RESOURCE_ID_ACTION_BAR_ICON_PAUSE);
  data->icon_stop = gbitmap_create_with_resource(RESOURCE_ID_ACTION_BAR_ICON_STOP);
  data->icon_more = gbitmap_create_with_resource(RESOURCE_ID_ACTION_BAR_ICON_MORE);

  window_init(&data->root_window, WINDOW_NAME("Stopwatch App"));
  window_set_user_data(&data->root_window, data);
  window_set_window_handlers(&data->root_window, &(WindowHandlers){
                                                   .load = prv_root_window_load,
                                                   .appear = prv_root_window_appear,
                                                   .unload = prv_root_window_unload,
                                                 });
  app_window_stack_push(&data->root_window, true /* animated */);

  app_event_loop();

  gbitmap_destroy(data->icon_start);
  gbitmap_destroy(data->icon_pause);
  gbitmap_destroy(data->icon_stop);
  gbitmap_destroy(data->icon_more);
  app_free(data);
}

const PebbleProcessMd *stopwatch_app_get_info(void) {
  static const PebbleProcessMdSystem s_stopwatch_app_info = {
    .common =
        {
          .main_func = &prv_main,
          .uuid = STOPWATCH_APP_UUID,
        },
    .name = i18n_noop("Stopwatch"),
    .icon_resource_id = RESOURCE_ID_STOPWATCH_TINY,
  };
  return (const PebbleProcessMd *)&s_stopwatch_app_info;
}

#endif // CONFIG_SERVICE_STOPWATCH
