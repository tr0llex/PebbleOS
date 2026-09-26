/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/wellbeing/wellbeing.h"

#include <pbl/logging/logging.h>
#include "pbl/services/accel_manager.h"
#include "pbl/services/activity/activity.h"
#include "pbl/services/light.h"
#include "pbl/services/regular_timer.h"
#include "pbl/services/system_task.h"
#include "shell/prefs.h"

#include "wellbeing_private.h"

PBL_LOG_MODULE_DECLARE(service_wellbeing, CONFIG_SERVICE_WELLBEING_LOG_LEVEL);

//! Dim enough to read the time by, dark enough not to wake a room.
#define NIGHT_CEILING_PCT 10

static bool s_initialized;
static bool s_active;

//! Ночной режим перепроверяется по времени, а не только по событиям сна.
//!
//! Раньше он менялся исключительно по HealthEventSleepUpdate. Если такие
//! события переставали приходить — счётчик сна закончил сессию, здоровье
//! выключили, метрика перестала читаться, — режим оставался включённым навсегда:
//! подсветка по движению выключена, яркость зажата десятью процентами. Человек
//! поднимает руку, а экран не загорается, и сделать с этим ничего нельзя, кроме
//! перезагрузки.
//!
//! Минута выбрана потому, что система и так тикает раз в минуту ради часов:
//! своих пробуждений эта проверка не добавляет.
#define RECHECK_PERIOD_MIN 1

static void prv_recheck_cb(void *data);
static RegularTimerInfo s_recheck = {.cb = prv_recheck_cb};
//! Таймер заведён. Пока режим выключен, его не должно быть вовсе: он будил бы
//! процессор раз в минуту ради проверки, ответ на которую известен заранее.
static bool s_recheck_running;

//! Завести или снять перепроверку по состоянию настройки.
static void prv_recheck_sync(bool wanted) {
  if (wanted == s_recheck_running) {
    return;
  }
  if (wanted) {
    regular_timer_add_multiminute_callback(&s_recheck, RECHECK_PERIOD_MIN);
  } else {
    regular_timer_remove_callback(&s_recheck);
  }
  s_recheck_running = wanted;
}

//! @param force write the settings even when the state has not changed. The
//! motion-wake preference is written straight to the accel manager when the
//! wearer edits it, which would undo our suppression; re-applying puts night
//! mode back on top, where it has to be.
static void prv_apply(bool active, bool force) {
  if (active == s_active && !force) {
    return;
  }
  s_active = active;

  light_set_night_ceiling(active ? NIGHT_CEILING_PCT : 0);
  // Rolling over is what the sleep tracker is watching; it must not also be
  // what turns the light on.
  accel_manager_set_motion_backlight_enabled(active ? false : backlight_is_motion_enabled());

  if (!force) {
    PBL_LOG_INFO("Night mode %s", active ? "on" : "off");
  }
}

static void prv_update(bool force) {
  if (!s_initialized) {
    return;
  }
  const bool wanted = backlight_is_night_mode_enabled();
  prv_recheck_sync(wanted);
  if (!wanted) {
    prv_apply(false, force);
    return;
  }

  int32_t sleep_state = ActivitySleepStateUnknown;
  if (!activity_get_metric(ActivityMetricSleepState, 1, &sleep_state)) {
    // Метрика не прочиталась — снимаем режим, а не оставляем как было. Ошибиться
    // в сторону обычного поведения часов безопасно, в сторону «спит» — нет:
    // именно так режим и застревал.
    prv_apply(false, force);
    return;
  }
  // Unknown is not asleep: without a reading, behave as the watch normally does.
  prv_apply(
      sleep_state == ActivitySleepStateLightSleep || sleep_state == ActivitySleepStateRestfulSleep,
      force);
}

void night_mode_handle_sleep_update(void) {
  prv_update(false);
}

static void prv_prefs_changed_task(void *unused) {
  prv_update(true);
}

void night_mode_handle_prefs_changed(void) {
  // Настройку меняют с разных задач: телефон — на KernelMain, меню настроек —
  // на задаче приложения. Состояние режима без блокировки, поэтому всё, что его
  // трогает, собираем на одной задаче — KernelBG, как и события сна.
  system_task_add_callback(prv_prefs_changed_task, NULL);
}

bool night_mode_is_active(void) {
  return s_active;
}

//! Чтение метрики и работа с настройками блокируют, поэтому не на таймерной
//! задаче.
static void prv_recheck_task(void *unused) {
  prv_update(false);
}

static void prv_recheck_cb(void *data) {
  system_task_add_callback(prv_recheck_task, NULL);
}

void night_mode_init(void) {
  s_initialized = true;
  // Таймер заводит сама prv_update, если режим включён.
  prv_update(false);
}
