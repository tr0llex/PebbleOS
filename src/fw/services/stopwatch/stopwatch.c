/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/stopwatch/stopwatch.h"

#include <pbl/drivers/rtc.h>
#include <pbl/logging/logging.h>
#include "pbl/kernel/mutex.h"
#include "pbl/services/i18n/i18n.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/notifications/notifications.h"
#include "pbl/services/system_task.h"
#include "pbl/services/timeline/attribute.h"
#include "pbl/services/timeline/item.h"
#include "pbl/util/uuid.h"
#include "resource/resource_ids.auto.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

PBL_LOG_MODULE_DEFINE(service_stopwatch, CONFIG_SERVICE_STOPWATCH_LOG_LEVEL);

//! The timer alert is a watch-generated notification; give it a source of its
//! own so the phone does not try to attribute it to an app it does not know.
#define UUID_STOPWATCH_DATA_SOURCE \
  {0x6b, 0x2f, 0x91, 0x40, 0x1c, 0x8e, 0x43, 0x7d, 0xa5, 0x30, 0x08, 0xf1, 0x9d, 0x22, 0x4e, 0x63}

// PBL_MUTEX_DEFINE даёт готовый статический мьютекс — отдельная
// инициализация ему не нужна.
static PBL_MUTEX_DEFINE(s_mutex);

// Stopwatch. Elapsed time is `accumulated` plus, while running, whatever has
// passed since `started_ticks`.
static bool s_running;
static RtcTicks s_started_ticks;
static uint32_t s_accumulated_ms;
static uint32_t s_lap_ms[STOPWATCH_MAX_LAPS];
static uint8_t s_lap_count;
static uint16_t s_lap_total;
//! Elapsed time at the previous lap, so a lap is a split and not a total.
static uint32_t s_last_lap_at_ms;

// Countdown. Same shape: `remaining_ms` is what was left when it was last
// paused, and the expiry timer is only armed while it runs.
static TimerID s_timer_id = TIMER_INVALID_ID;
static bool s_timer_set;
static bool s_timer_running;
static RtcTicks s_timer_started_ticks;
static uint32_t s_timer_remaining_ms;
static uint32_t s_timer_duration_s;
//! Bumped by every start and cancel. The expiry alert carries the generation it
//! was raised for, so a countdown cancelled in the moment between the timer
//! firing and the alert being built does not go off anyway.
static uint32_t s_timer_generation;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Helpers

static uint32_t prv_ticks_to_ms(RtcTicks ticks) {
  return (uint32_t)((ticks * 1000) / RTC_TICKS_HZ);
}

static uint32_t prv_elapsed_ms_locked(void) {
  if (!s_running) {
    return s_accumulated_ms;
  }
  return s_accumulated_ms + prv_ticks_to_ms(rtc_get_ticks() - s_started_ticks);
}

static uint32_t prv_timer_remaining_ms_locked(void) {
  if (!s_timer_running) {
    return s_timer_remaining_ms;
  }
  const uint32_t gone_ms = prv_ticks_to_ms(rtc_get_ticks() - s_timer_started_ticks);
  return (gone_ms >= s_timer_remaining_ms) ? 0 : (s_timer_remaining_ms - gone_ms);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Expiry

//! Runs on KernelBG: building a timeline item allocates and talks to the
//! notification service, neither of which belongs on the timer task.
static void prv_notify_expired(void *generation) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const bool stale = ((uintptr_t)generation != s_timer_generation);
  pbl_mutex_unlock(&s_mutex);
  if (stale) {
    return;
  }

  AttributeList attr_list = {0};
  attribute_list_add_resource_id(&attr_list, AttributeIdIconTiny, RESOURCE_ID_ALARM_CLOCK_TINY);
  attribute_list_add_cstring(&attr_list, AttributeIdTitle,
                             i18n_get(i18n_noop("Timer"), &attr_list));
  attribute_list_add_cstring(&attr_list, AttributeIdBody,
                             i18n_get(i18n_noop("Time is up"), &attr_list));
  attribute_list_add_uint8(&attr_list, AttributeIdBgColor, GColorRedARGB8);

  TimelineItem *item = timeline_item_create_with_attributes(
      rtc_get_time(), 0, TimelineItemTypeNotification, LayoutIdNotification, &attr_list, NULL);
  if (item) {
    item->header.from_watch = true;
    item->header.parent_id = (Uuid)UUID_STOPWATCH_DATA_SOURCE;
    notifications_add_notification(item);
    timeline_item_destroy(item);
  }

  i18n_free_all(&attr_list);
  attribute_list_destroy_list(&attr_list);
}

static void prv_timer_expired_cb(void *armed_generation) {
  uint32_t generation;

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  {
    // A pause, cancel or restart can race the timer task; only alert if the
    // countdown that armed this callback is still the one that is running. The
    // generation is the one the timer was armed with: a restart while this
    // callback waited on the mutex bumps it, and the new countdown must not be
    // declared expired the moment it starts.
    if (!s_timer_running || (uintptr_t)armed_generation != s_timer_generation) {
      pbl_mutex_unlock(&s_mutex);
      return;
    }
    s_timer_running = false;
    s_timer_remaining_ms = 0;
    generation = s_timer_generation;
    // Queued under the lock so the generation it carries cannot go stale
    // between reading it and handing it over.
    system_task_add_callback(prv_notify_expired, (void *)(uintptr_t)generation);
  }
  pbl_mutex_unlock(&s_mutex);

  PBL_LOG_INFO("Timer expired");
}

//! Arm or disarm the expiry timer to match the current countdown state.
static void prv_rearm_locked(void) {
  if (s_timer_id == TIMER_INVALID_ID) {
    return;
  }
  new_timer_stop(s_timer_id);
  if (s_timer_running && s_timer_remaining_ms > 0) {
    new_timer_start(s_timer_id, s_timer_remaining_ms, prv_timer_expired_cb,
                    (void *)(uintptr_t)s_timer_generation, 0);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Stopwatch API

void stopwatch_start(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  if (!s_running) {
    s_started_ticks = rtc_get_ticks();
    s_running = true;
  }
  pbl_mutex_unlock(&s_mutex);
}

void stopwatch_pause(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  if (s_running) {
    s_accumulated_ms = prv_elapsed_ms_locked();
    s_running = false;
  }
  pbl_mutex_unlock(&s_mutex);
}

void stopwatch_reset(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  s_running = false;
  s_accumulated_ms = 0;
  s_last_lap_at_ms = 0;
  s_lap_count = 0;
  s_lap_total = 0;
  memset(s_lap_ms, 0, sizeof(s_lap_ms));
  pbl_mutex_unlock(&s_mutex);
}

bool stopwatch_is_running(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const bool running = s_running;
  pbl_mutex_unlock(&s_mutex);
  return running;
}

uint32_t stopwatch_get_elapsed_ms(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const uint32_t elapsed = prv_elapsed_ms_locked();
  pbl_mutex_unlock(&s_mutex);
  return elapsed;
}

void stopwatch_lap(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  if (s_running) {
    const uint32_t now_ms = prv_elapsed_ms_locked();
    // Newest first: the lap the wearer just took is the one they want to read.
    for (int i = STOPWATCH_MAX_LAPS - 1; i > 0; i--) {
      s_lap_ms[i] = s_lap_ms[i - 1];
    }
    s_lap_ms[0] = now_ms - s_last_lap_at_ms;
    s_last_lap_at_ms = now_ms;
    if (s_lap_count < STOPWATCH_MAX_LAPS) {
      s_lap_count++;
    }
    if (s_lap_total < UINT16_MAX) {
      s_lap_total++;
    }
  }
  pbl_mutex_unlock(&s_mutex);
}

uint8_t stopwatch_get_lap_count(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const uint8_t count = s_lap_count;
  pbl_mutex_unlock(&s_mutex);
  return count;
}

uint16_t stopwatch_get_lap_total(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const uint16_t total = s_lap_total;
  pbl_mutex_unlock(&s_mutex);
  return total;
}

uint32_t stopwatch_get_lap_ms(uint8_t index) {
  uint32_t lap = 0;
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  if (index < s_lap_count) {
    lap = s_lap_ms[index];
  }
  pbl_mutex_unlock(&s_mutex);
  return lap;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Countdown API

void stopwatch_timer_start(uint32_t duration_s) {
  if (duration_s == 0 || duration_s > STOPWATCH_TIMER_MAX_S) {
    return;
  }
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  s_timer_duration_s = duration_s;
  s_timer_remaining_ms = duration_s * 1000;
  s_timer_started_ticks = rtc_get_ticks();
  s_timer_set = true;
  s_timer_running = true;
  s_timer_generation++;
  prv_rearm_locked();
  pbl_mutex_unlock(&s_mutex);
}

bool stopwatch_timer_is_set(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const bool set = s_timer_set;
  pbl_mutex_unlock(&s_mutex);
  return set;
}

bool stopwatch_timer_is_running(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const bool running = s_timer_running;
  pbl_mutex_unlock(&s_mutex);
  return running;
}

void stopwatch_timer_pause(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  if (s_timer_running) {
    s_timer_remaining_ms = prv_timer_remaining_ms_locked();
    s_timer_running = false;
    prv_rearm_locked();
  }
  pbl_mutex_unlock(&s_mutex);
}

void stopwatch_timer_resume(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  if (s_timer_set && !s_timer_running && s_timer_remaining_ms > 0) {
    s_timer_started_ticks = rtc_get_ticks();
    s_timer_running = true;
    prv_rearm_locked();
  }
  pbl_mutex_unlock(&s_mutex);
}

void stopwatch_timer_cancel(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  s_timer_set = false;
  s_timer_running = false;
  s_timer_remaining_ms = 0;
  s_timer_generation++;
  prv_rearm_locked();
  pbl_mutex_unlock(&s_mutex);
}

uint32_t stopwatch_timer_get_remaining_ms(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const uint32_t remaining = prv_timer_remaining_ms_locked();
  pbl_mutex_unlock(&s_mutex);
  return remaining;
}

uint32_t stopwatch_timer_get_duration_s(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const uint32_t duration = s_timer_duration_s;
  pbl_mutex_unlock(&s_mutex);
  return duration;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void stopwatch_service_init(void) {
  s_timer_id = new_timer_create();
  if (s_timer_id == TIMER_INVALID_ID) {
    PBL_LOG_ERR("Failed to create the countdown timer");
  }
}
