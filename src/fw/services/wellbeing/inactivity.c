/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/wellbeing/wellbeing.h"

#include "wellbeing_private.h"

#include <pbl/drivers/rtc.h>
#include <pbl/logging/logging.h>
#include "pbl/services/activity/activity.h"
#include "pbl/services/activity/workout_service.h"
#include "pbl/services/i18n/i18n.h"
#include "pbl/services/notifications/do_not_disturb.h"
#include "pbl/services/notifications/notifications.h"
#include "pbl/services/regular_timer.h"
#include "pbl/services/system_task.h"
#include "pbl/services/timeline/attribute.h"
#include "pbl/services/timeline/item.h"
#include "pbl/util/uuid.h"
#include "resource/resource_ids.auto.h"
#include "shell/prefs.h"
#include "util/time/time.h"

#include <inttypes.h>
#include <string.h>

PBL_LOG_MODULE_DECLARE(service_wellbeing, CONFIG_SERVICE_WELLBEING_LOG_LEVEL);

//! Watch-generated, like the health insights: give it a source of its own.
#define UUID_WELLBEING_DATA_SOURCE \
  {0x9a, 0x14, 0x77, 0x2b, 0x35, 0xd0, 0x4e, 0x18, 0xbb, 0x62, 0x0c, 0x93, 0x71, 0x5a, 0x2f, 0x40}

//! Steps are sampled on this cadence and the window is this many samples long,
//! so the reminder looks back exactly an hour.
#define SAMPLE_PERIOD_MIN 5
#define WINDOW_SAMPLES    (60 / SAMPLE_PERIOD_MIN)

//! Fewer steps than this over the window counts as not having moved. An hour of
//! desk work is well under it; a trip to the kitchen clears it.
#define WINDOW_MIN_STEPS 100

//! Quiet outside these hours: nobody wants to be told to stand up at dawn.
#define EARLIEST_HOUR 9
#define LATEST_HOUR   21

//! At most one reminder per hour, so ignoring one does not mean being nagged.
#define MIN_INTERVAL_S (60 * 60)

//! No sample yet in this slot.
#define SAMPLE_NONE UINT16_MAX

static uint16_t s_samples[WINDOW_SAMPLES];
static uint8_t s_next_slot;
static time_t s_last_nudge_utc;
//! Таймер заведён. Пока напоминание выключено, его не должно быть вовсе:
//! пробуждение раз в пять минут ради проверки «а выключено ли» — плата ни за
//! что, а часы должны держать заряд неделями.
static bool s_timer_running;

static void prv_timer_cb(void *data);
static RegularTimerInfo s_timer_info = {.cb = prv_timer_cb};

////////////////////////////////////////////////////////////////////////////////////////////////////

static void prv_push_notification(void *unused) {
  AttributeList attr_list = {0};
  attribute_list_add_resource_id(&attr_list, AttributeIdIconTiny,
                                 RESOURCE_ID_REACHED_FITNESS_GOAL_TINY);
  attribute_list_add_cstring(&attr_list, AttributeIdTitle,
                             i18n_get(i18n_noop("Time to move"), &attr_list));
  attribute_list_add_cstring(&attr_list, AttributeIdBody,
                             i18n_get(i18n_noop("You have been still for an hour. A short walk "
                                                "would do you good."),
                                      &attr_list));
  attribute_list_add_uint8(&attr_list, AttributeIdBgColor, GColorOrangeARGB8);

  TimelineItem *item = timeline_item_create_with_attributes(
      rtc_get_time(), 0, TimelineItemTypeNotification, LayoutIdNotification, &attr_list, NULL);
  if (item) {
    item->header.from_watch = true;
    item->header.parent_id = (Uuid)UUID_WELLBEING_DATA_SOURCE;
    notifications_add_notification(item);
    timeline_item_destroy(item);
  }

  i18n_free_all(&attr_list);
  attribute_list_destroy_list(&attr_list);
}

//! Steps taken over the window, or -1 while the window is not full yet.
static int32_t prv_window_steps(uint16_t current) {
  // The oldest sample is the one about to be overwritten.
  const uint16_t oldest = s_samples[s_next_slot];
  if (oldest == SAMPLE_NONE) {
    return -1;
  }
  // Midnight resets the daily counter; a negative delta means the window
  // straddles it and says nothing useful about the last hour.
  if (current < oldest) {
    return -1;
  }
  return current - oldest;
}

static void prv_evaluate(void *unused) {
  if (!move_reminder_is_enabled() || !activity_prefs_tracking_is_enabled()) {
    return;
  }

  int32_t steps = 0;
  if (!activity_get_metric(ActivityMetricStepCount, 1, &steps)) {
    return;
  }
  const uint16_t current = (steps > UINT16_MAX - 1) ? (UINT16_MAX - 1) : (uint16_t)steps;
  const int32_t window_steps = prv_window_steps(current);

  s_samples[s_next_slot] = current;
  s_next_slot = (s_next_slot + 1) % WINDOW_SAMPLES;

  if (window_steps < 0 || window_steps >= WINDOW_MIN_STEPS) {
    return;
  }

  const time_t now = rtc_get_time();
  if (s_last_nudge_utc != 0 && (now - s_last_nudge_utc) < MIN_INTERVAL_S) {
    return;
  }

  struct tm local;
  localtime_r(&now, &local);
  if (local.tm_hour < EARLIEST_HOUR || local.tm_hour >= LATEST_HOUR) {
    return;
  }

  // Every one of these means the wearer already knows what they are doing.
  if (night_mode_is_active() || do_not_disturb_is_active() ||
      workout_service_is_workout_ongoing()) {
    return;
  }

  int32_t sleep_state = ActivitySleepStateUnknown;
  if (activity_get_metric(ActivityMetricSleepState, 1, &sleep_state) &&
      sleep_state != ActivitySleepStateAwake) {
    return;
  }

  s_last_nudge_utc = now;
  PBL_LOG_INFO("Move reminder: %" PRId32 " steps in the last hour", window_steps);
  prv_push_notification(NULL);
}

static void prv_timer_cb(void *data) {
  // The regular timer runs on the timer task; reading activity metrics and
  // building a notification belong on KernelBG.
  system_task_add_callback(prv_evaluate, NULL);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

//! Завести или снять опрос шагов по состоянию настройки.
//!
//! Окно счётчика сбрасывается при каждом включении: пока напоминание было
//! выключено, шаги шли мимо, и разность с последним запомненным значением
//! означала бы не «час без движения», а всё пропущенное время.
static void prv_sync_timer(void *unused) {
  const bool wanted = move_reminder_is_enabled();
  if (wanted == s_timer_running) {
    return;
  }
  if (wanted) {
    for (int i = 0; i < WINDOW_SAMPLES; i++) {
      s_samples[i] = SAMPLE_NONE;
    }
    s_next_slot = 0;
    s_last_nudge_utc = 0;
    regular_timer_add_multiminute_callback(&s_timer_info, SAMPLE_PERIOD_MIN);
  } else {
    regular_timer_remove_callback(&s_timer_info);
  }
  s_timer_running = wanted;
}

void inactivity_handle_prefs_changed(void) {
  // Как и ночной режим: настройку меняют с разных задач, а состояние опроса
  // без блокировки — применяем на KernelBG.
  system_task_add_callback(prv_sync_timer, NULL);
}

void inactivity_init(void) {
  for (int i = 0; i < WINDOW_SAMPLES; i++) {
    s_samples[i] = SAMPLE_NONE;
  }
  s_next_slot = 0;
  s_last_nudge_utc = 0;
  prv_sync_timer(NULL);
}
