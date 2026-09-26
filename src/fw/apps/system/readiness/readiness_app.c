/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "readiness_app.h"

// Сервис может быть выключен в конфигурации, а CMake собирает все файлы
// каталога: без этой обёртки сборка без сервиса падала бы на ссылках.
#ifdef CONFIG_SERVICE_WELLBEING

#include "applib/app.h"
#include "applib/fonts/fonts.h"
#include "applib/graphics/graphics.h"
#include "applib/ui/animation.h"
#include "applib/ui/ui.h"
#include "apps/system/common/app_text_util.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "pbl/services/activity/activity.h"
#include "pbl/services/i18n/i18n.h"
#include "pbl/services/wellbeing/wellbeing.h"
#include "pbl/util/math.h"

#include <inttypes.h>
#include <stdio.h>

// Bands measured off screenshots: the LECO 42 score inks 29 px, the GOTHIC 24
// BOLD phrase 18 px, the GOTHIC 18 detail line 11 px. The group is centred,
// leaving about 28 px above and below.
#define SCORE_TOP  14
#define SCORE_H    56
#define PHRASE_TOP 76
#define PHRASE_H   28
#define DETAIL_TOP 110
#define DETAIL_H   22

#define CHART_TOP     146
#define CHART_H       52
#define CHART_MARGIN  12
#define CHART_BAR_GAP 3

typedef struct ReadinessNight {
  uint8_t score;
  uint16_t rmssd_ms;
  uint8_t resting_bpm;
  bool valid;
} ReadinessNightView;

// The score counts up and the bars grow when the screen appears: the number is
// the answer to "how did I sleep", and watching it arrive makes it land instead
// of being one more figure that was already on screen.
#define REVEAL_MS 700

typedef struct ReadinessAppData {
  Animation *reveal;
  //! 0..ANIMATION_NORMALIZED_MAX; scales the score and the bar heights.
  AnimationProgress reveal_progress;

  Window window;
  Layer layer;

  ReadinessNightView nights[READINESS_HISTORY_NIGHTS];
  uint8_t night_count;

  char score_text[8];
  char detail_text[48];
} ReadinessAppData;

static void prv_load_nights(ReadinessAppData *data) {
  data->night_count = 0;
  for (uint8_t i = 0; i < READINESS_HISTORY_NIGHTS; i++) {
    ReadinessNightView *night = &data->nights[i];
    night->valid =
        readiness_get_night(i, NULL, &night->rmssd_ms, &night->resting_bpm, &night->score);
    if (!night->valid) {
      break;
    }
    data->night_count++;
  }
}

//! Bars for the nights that have a score, newest on the right so the chart
//! reads the way time does.
static void prv_draw_chart(GContext *ctx, ReadinessAppData *data, GRect bounds) {
  const GRect area = GRect(CHART_MARGIN, CHART_TOP, bounds.size.w - 2 * CHART_MARGIN, CHART_H);

  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_line(ctx, GPoint(area.origin.x, area.origin.y + area.size.h),
                     GPoint(area.origin.x + area.size.w, area.origin.y + area.size.h));

  if (data->night_count == 0) {
    return;
  }

  const int16_t slot_w = area.size.w / READINESS_HISTORY_NIGHTS;
  const int16_t bar_w = MAX(slot_w - CHART_BAR_GAP, 2);

  for (uint8_t i = 0; i < data->night_count; i++) {
    const ReadinessNightView *night = &data->nights[i];
    if (night->score == 0) {
      continue;
    }
    // Index 0 is the newest night, and it belongs at the right-hand end.
    const int16_t slot = READINESS_HISTORY_NIGHTS - 1 - i;
    const int16_t full_h = MAX((int16_t)((int32_t)area.size.h * night->score / 100), 2);
    const int16_t h =
        MAX((int16_t)(((int32_t)full_h * data->reveal_progress) / ANIMATION_NORMALIZED_MAX), 1);
    const GRect bar =
        GRect(area.origin.x + slot * slot_w, area.origin.y + area.size.h - h, bar_w, h);
    graphics_context_set_fill_color(ctx, (i == 0) ? GColorJaegerGreen : GColorLightGray);
    graphics_fill_rect(ctx, &bar);
  }
}

static void prv_reveal_update(Animation *animation, const AnimationProgress progress) {
  ReadinessAppData *data = animation_get_context(animation);
  data->reveal_progress = progress;
  layer_mark_dirty(&data->layer);
}

//! Отыгравшая анимация уничтожается сама; указатель обнуляем, чтобы выход из
//! приложения не звал animation_destroy на уже удалённую.
static void prv_reveal_stopped(Animation *animation, bool finished, void *context) {
  ReadinessAppData *data = context;
  if (data->reveal == animation) {
    data->reveal = NULL;
  }
}

static const AnimationImplementation s_reveal_impl = {
  .update = prv_reveal_update,
};

//! Экран первых ночей: оценки ещё нет, но замеры уже есть — и это надо
//! показать, иначе приложение выглядит сломанным.
static void prv_draw_measured(GContext *ctx, ReadinessAppData *data, GRect bounds,
                              const ReadinessNightView *latest) {
  // Вариабельность за ночь набирается не всегда: датчику нужен спокойный
  // контакт. Тогда от ночи остаётся пульс покоя — его и показываем, чтобы было
  // видно, что замер был.
  const bool has_hrv = (latest->rmssd_ms > 0);

  // Единицы уходят в подпись, а не к числу: крупное число рисуется шрифтом
  // LECO, а в нём нет кириллицы — «мс» выходило двумя плашками.
  snprintf(data->detail_text, sizeof(data->detail_text), "%" PRIu16,
           has_hrv ? latest->rmssd_ms : (uint16_t)latest->resting_bpm);
  app_draw_text_centred(ctx, data->detail_text, fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS),
                        GRect(0, SCORE_TOP, bounds.size.w, SCORE_H));

  app_draw_text_centred(
      ctx, i18n_get(has_hrv ? i18n_noop("HRV overnight, ms") : i18n_noop("Resting pulse"), data),
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
      GRect(0, PHRASE_TOP, bounds.size.w, PHRASE_H));

  if (has_hrv) {
    snprintf(data->detail_text, sizeof(data->detail_text), "%s %" PRIu8,
             i18n_get(i18n_noop("Pulse"), data), latest->resting_bpm);
  } else {
    i18n_get_with_buffer(i18n_noop("No HRV that night"), data->detail_text,
                         sizeof(data->detail_text));
  }
  app_draw_text_centred(ctx, data->detail_text, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                        GRect(0, DETAIL_TOP, bounds.size.w, DETAIL_H));

  // Оценке нужна точка отсчёта: медиана прошлых ночей. В неё идут только ночи
  // с вариабельностью — ночь, где датчик набрал один пульс покоя, записана и
  // видна, но точку отсчёта не двигает. Считать её здесь значило бы обещать
  // оценку раньше, чем она появится, и счётчик застревал бы на «осталось 1».
  // Сегодняшняя ночь в точку отсчёта тоже не входит, отсюда +1.
  int with_hrv = 0;
  for (uint8_t i = 0; i < data->night_count; i++) {
    if (data->nights[i].rmssd_ms > 0) {
      with_hrv++;
    }
  }
  const int left = READINESS_BASELINE_NIGHTS + 1 - with_hrv;
  snprintf(data->detail_text, sizeof(data->detail_text), "%s %d",
           i18n_get(i18n_noop("Nights to go:"), data), left > 0 ? left : 1);
  app_draw_text_centred(ctx, data->detail_text, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                        GRect(0, CHART_TOP, bounds.size.w, DETAIL_H));
}

static void prv_update_proc(Layer *layer, GContext *ctx) {
  ReadinessAppData *data = window_get_user_data(layer_get_window(layer));
  const GRect bounds = layer->bounds;

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, &bounds);
  graphics_context_set_text_color(ctx, GColorBlack);

  const ReadinessNightView *latest = &data->nights[0];

  if (!latest->valid) {
    // Ни одной ночи не записано. Причин ровно две, и они требуют разных
    // действий, поэтому и говорим по-разному: либо пульс выключен в настройках
    // и мерить нечем, либо просто ещё не было ночи.
    const char *message = activity_prefs_heart_rate_is_enabled()
                              ? i18n_noop("No readings yet. Wear the watch overnight.")
                              : i18n_noop("Turn on heart rate in Health settings.");
    const GRect box = grect_inset(bounds, GEdgeInsets(bounds.size.h / 3, 12, 0, 12));
    graphics_draw_text(ctx, i18n_get(message, data), fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                       box, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  if (latest->score == 0) {
    // Замеры есть, а оценки нет: её не с чем сравнивать, пока не набралось
    // ночей на точку отсчёта. Раньше здесь показывалось «замеров нет» — и это
    // была неправда, из-за которой владелец решил, что ничего не работает.
    // Показываем то, что уже измерено, и сколько ночей осталось.
    prv_draw_measured(ctx, data, bounds, latest);
    return;
  }

  const uint8_t shown_score =
      (uint8_t)(((uint32_t)latest->score * data->reveal_progress) / ANIMATION_NORMALIZED_MAX);
  snprintf(data->score_text, sizeof(data->score_text), "%" PRIu8, shown_score);
  app_draw_text_centred(ctx, data->score_text, fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS),
                        GRect(0, SCORE_TOP, bounds.size.w, SCORE_H));

  const char *phrase;
  if (latest->score >= 70) {
    phrase = i18n_noop("Well recovered");
  } else if (latest->score >= 40) {
    phrase = i18n_noop("About as usual");
  } else {
    phrase = i18n_noop("Take it easy today");
  }
  app_draw_text_centred(ctx, i18n_get(phrase, data), fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                        GRect(0, PHRASE_TOP, bounds.size.w, PHRASE_H));

  snprintf(data->detail_text, sizeof(data->detail_text), "%s %" PRIu16 " %s   %s %" PRIu8,
           i18n_get(i18n_noop("HRV"), data), latest->rmssd_ms,
           i18n_get(i18n_ctx_noop("Duration", "ms"), data), i18n_get(i18n_noop("Pulse"), data),
           latest->resting_bpm);
  app_draw_text_centred(ctx, data->detail_text, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                        GRect(0, DETAIL_TOP, bounds.size.w, DETAIL_H));

  prv_draw_chart(ctx, data, bounds);
}

static void prv_window_load(Window *window) {
  ReadinessAppData *data = window_get_user_data(window);

  prv_load_nights(data);

  Layer *root = window_get_root_layer(window);
  layer_init(&data->layer, &root->bounds);
  layer_set_update_proc(&data->layer, prv_update_proc);
  layer_add_child(root, &data->layer);

  // Nothing to count up to when there is no night on record; leave the reveal
  // at full so the "wear it overnight" message is not animated in from nothing.
  if (data->night_count == 0) {
    data->reveal_progress = ANIMATION_NORMALIZED_MAX;
    return;
  }

  data->reveal = animation_create();
  if (data->reveal) {
    animation_set_implementation(data->reveal, &s_reveal_impl);
    animation_set_handlers(data->reveal, (AnimationHandlers){.stopped = prv_reveal_stopped}, data);
    animation_set_duration(data->reveal, REVEAL_MS);
    animation_set_curve(data->reveal, AnimationCurveEaseOut);
    animation_schedule(data->reveal);
  } else {
    data->reveal_progress = ANIMATION_NORMALIZED_MAX;
  }
}

static void prv_window_unload(Window *window) {
  ReadinessAppData *data = window_get_user_data(window);
  if (data->reveal) {
    // Живая анимация уничтожается при снятии сама (auto_destroy), а обработчик
    // stopped обнуляет указатель; отдельный animation_destroy был бы вторым.
    animation_unschedule(data->reveal);
    data->reveal = NULL;
  }
  i18n_free_all(data);
}

static void prv_main(void) {
  ReadinessAppData *data = app_zalloc_check(sizeof(ReadinessAppData));
  app_state_set_user_data(data);

  window_init(&data->window, WINDOW_NAME("Readiness"));
  window_set_user_data(&data->window, data);
  window_set_window_handlers(&data->window, &(WindowHandlers){
                                              .load = prv_window_load,
                                              .unload = prv_window_unload,
                                            });
  app_window_stack_push(&data->window, true /* animated */);

  app_event_loop();

  app_free(data);
}

const PebbleProcessMd *readiness_app_get_info(void) {
  static const PebbleProcessMdSystem s_readiness_app_info = {
    .common =
        {
          .main_func = &prv_main,
          .uuid = READINESS_APP_UUID,
        },
    .name = i18n_noop("Readiness"),
    .icon_resource_id = RESOURCE_ID_HEART_TINY,
  };
  return (const PebbleProcessMd *)&s_readiness_app_info;
}

#endif // CONFIG_SERVICE_WELLBEING
