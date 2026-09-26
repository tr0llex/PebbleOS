/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

//! Ночной замер готовности как машина состояний во времени: подписались на
//! пульсометр, набрали удары, закончили, записали ночь, на следующую ночь
//! начали заново. Каждое звено здесь проверяется без часов на руке — в
//! эмуляторе пульсометра нет, а на живых часах поломка видна только через
//! трое суток по застывшему экрану.

#include "pbl/services/wellbeing/wellbeing.h"
#include "pbl/services/hrm/hrm_manager.h"
#include "pbl/services/hrm/hrm_manager_private.h"
#include "pbl/services/activity/activity.h"

#include "clar.h"

#include <string.h>

// Stubs
////////////////////////////////////////////////////////////////
#include "stubs_attribute.h"
#include "stubs_i18n.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_notifications.h"
#include "stubs_passert.h"
#include "stubs_regular_timer.h"
#include "stubs_timeline_item.h"

// Fakes
////////////////////////////////////////////////////////////////
#include "fake_pbl_malloc.h"
#include "fake_rtc.h"
#include "fake_settings_file.h"
#include "fake_system_task.h"

void readiness_init(void);
void readiness_handle_sleep_update(void);

// Пульсометр: запоминаем подписку и колбэк, события шлёт сам тест.
static HRMSubscriberCallback s_hrm_cb;
static HRMSessionRef s_hrm_session;
static int s_subscribe_count;
static int s_unsubscribe_count;

bool sys_hrm_manager_is_hrm_present(void) {
  return true;
}

HRMSessionRef hrm_manager_subscribe_with_callback(AppInstallId app_id, uint32_t update_interval_s,
                                                  uint16_t expire_s, HRMFeature features,
                                                  bool low_latency, HRMSubscriberCallback callback,
                                                  void *context) {
  s_subscribe_count++;
  s_hrm_cb = callback;
  s_hrm_session = 100 + s_subscribe_count;
  return s_hrm_session;
}

bool sys_hrm_manager_unsubscribe(HRMSessionRef session) {
  s_unsubscribe_count++;
  // Подписка, которую менеджер уже удалил сам, — законный случай: так
  // ведёт себя настоящий hrm_manager.c, возвращая false.
  const bool known = (session == s_hrm_session);
  s_hrm_session = HRM_INVALID_SESSION_REF;
  return known;
}

// Сон: какую стадию отдаёт счётчик активности.
static int32_t s_sleep_state;

bool activity_get_metric(ActivityMetric metric, uint32_t history_len, int32_t *history) {
  if (metric != ActivityMetricSleepState) {
    return false;
  }
  history[0] = s_sleep_state;
  return true;
}

bool activity_prefs_heart_rate_is_enabled(void) {
  return true;
}

// Helpers
////////////////////////////////////////////////////////////////

#define DAY_S (24 * 60 * 60)
//! 1 января 2027, 00:00 UTC. Часовой пояс в тестах прошивки — UTC.
#define JAN_1_2027 1798761600

static void prv_set_time(time_t utc) {
  fake_rtc_init(0, utc);
}

static void prv_sleep_event(int32_t state) {
  s_sleep_state = state;
  readiness_handle_sleep_update();
  fake_system_task_callbacks_invoke_pending();
}

static void prv_send_bpm(uint8_t bpm) {
  cl_assert(s_hrm_cb);
  PebbleHRMEvent event = {
    .event_type = HRMEvent_BPM,
    .bpm = {.bpm = bpm, .quality = HRMQuality_Good},
  };
  s_hrm_cb(&event, NULL);
}

static void prv_send_ppi_q(uint16_t ppi_ms, HRMQuality quality) {
  cl_assert(s_hrm_cb);
  PebbleHRMEvent event = {
    .event_type = HRMEvent_HRV,
    .hrv = {.ppi_ms = ppi_ms, .quality = quality},
  };
  s_hrm_cb(&event, NULL);
}

static void prv_send_ppi(uint16_t ppi_ms) {
  prv_send_ppi_q(ppi_ms, HRMQuality_Good);
}

//! Спокойная ночь: 130 ударов с чередованием ±20 мс — RMSSD ровно 40.
static void prv_good_night(uint8_t bpm) {
  prv_send_bpm(bpm);
  for (int i = 0; i < 130; i++) {
    prv_send_ppi((i % 2) ? 1000 : 1040);
  }
  fake_system_task_callbacks_invoke_pending();
}

void test_readiness__initialize(void) {
  fake_settings_file_reset();
  fake_system_task_callbacks_cleanup();
  s_hrm_cb = NULL;
  s_hrm_session = HRM_INVALID_SESSION_REF;
  s_subscribe_count = 0;
  s_unsubscribe_count = 0;
  s_sleep_state = ActivitySleepStateAwake;
  readiness_init();
}

void test_readiness__cleanup(void) {
  fake_system_task_callbacks_cleanup();
}

// Tests
////////////////////////////////////////////////////////////////

//! Обычная ночь: удары набрались — подписка снята, ночь записана.
void test_readiness__good_night_is_recorded(void) {
  prv_set_time(JAN_1_2027 + 2 * 60 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  cl_assert_equal_i(s_subscribe_count, 1);

  prv_good_night(52);

  cl_assert_equal_i(s_unsubscribe_count, 1);
  uint16_t rmssd = 0;
  uint8_t bpm = 0;
  cl_assert(readiness_get_night(0, NULL, &rmssd, &bpm, NULL));
  cl_assert_equal_i(rmssd, 40);
  cl_assert_equal_i(bpm, 52);
}

//! Главное: ночь, в которой подписка истекла, а события «истекает» не пришло.
//!
//! Так бывает на плохом контакте. Менеджер пульсометра шлёт это событие
//! подписчикам KernelBG только из обработчика данных, а данных нет: датчик,
//! не набравший годного пульса за окно обслуживания, выключается
//! («HRM on for Ns without a valid reading, deferring subscribers»). Потом
//! подписка молча удаляется по сроку. Сервис же считал замер идущим вечно и
//! уходил на первой проверке каждую следующую ночь — до перезагрузки часов.
void test_readiness__expired_subscription_does_not_block_next_night(void) {
  // Ночь первая: пульс поймали, ударов для вариабельности не набрали, а
  // окончания подписки часы так и не сообщили.
  prv_set_time(JAN_1_2027 + 2 * 60 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  cl_assert_equal_i(s_subscribe_count, 1);
  prv_send_bpm(55);

  // Прошло больше срока подписки.
  prv_set_time(JAN_1_2027 + 2 * 60 * 60 + 15 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);

  // Замер должен считаться законченным и записанным, пусть без вариабельности.
  uint16_t day_id = 0;
  uint16_t rmssd = 99;
  uint8_t bpm = 0;
  cl_assert(readiness_get_night(0, &day_id, &rmssd, &bpm, NULL));
  cl_assert_equal_i(rmssd, 0);
  cl_assert_equal_i(bpm, 55);

  // Ночь вторая: замер обязан начаться заново.
  prv_set_time(JAN_1_2027 + DAY_S + 2 * 60 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  cl_assert_equal_i(s_subscribe_count, 2);

  prv_good_night(50);
  uint16_t second_day_id = 0;
  cl_assert(readiness_get_night(0, &second_day_id, &rmssd, &bpm, NULL));
  cl_assert(second_day_id != day_id);
  cl_assert_equal_i(rmssd, 40);
}

//! Зависший замер снимается и тогда, когда следующее событие сна — уже
//! пробуждение: иначе утро пришло бы без записи о ночи.
void test_readiness__expired_subscription_is_finished_on_wake(void) {
  prv_set_time(JAN_1_2027 + 3 * 60 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  prv_send_bpm(58);

  prv_set_time(JAN_1_2027 + 7 * 60 * 60);
  prv_sleep_event(ActivitySleepStateAwake);

  uint8_t bpm = 0;
  cl_assert(readiness_get_night(0, NULL, NULL, &bpm, NULL));
  cl_assert_equal_i(bpm, 58);
}

//! Замер, который ещё в пределах срока, трогать нельзя: иначе каждое событие
//! сна обрывало бы только что начатую попытку.
void test_readiness__running_measurement_is_left_alone(void) {
  prv_set_time(JAN_1_2027 + 2 * 60 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  prv_send_bpm(55);

  prv_set_time(JAN_1_2027 + 2 * 60 * 60 + 3 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);

  cl_assert_equal_i(s_subscribe_count, 1);
  cl_assert_equal_i(s_unsubscribe_count, 0);
  cl_assert(!readiness_get_night(0, NULL, NULL, NULL, NULL));
}

//! Плохой контакт: датчик горит две минуты и не отдаёт ничего. Дальше держать
//! его незачем — подписка снимается, не дожидаясь своих десяти минут.
//!
//! Защита менеджера здесь не срабатывает: продержав датчик без годного
//! показания, он даёт подписчику «выждать свой интервал», а наш интервал —
//! одна секунда. Поэтому срок держим сами.
void test_readiness__bad_contact_releases_the_hrm_early(void) {
  prv_set_time(JAN_1_2027 + 2 * 60 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  cl_assert_equal_i(s_subscribe_count, 1);
  cl_assert_equal_i(s_unsubscribe_count, 0);

  // Полторы минуты без данных — ещё рано, контакт мог появиться.
  prv_set_time(JAN_1_2027 + 2 * 60 * 60 + 90);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  cl_assert_equal_i(s_unsubscribe_count, 0);

  // Две минуты — и ни одного удара. Отпускаем датчик.
  prv_set_time(JAN_1_2027 + 2 * 60 * 60 + 120);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  cl_assert_equal_i(s_unsubscribe_count, 1);
  // Записывать нечего: ни ударов, ни пульса.
  cl_assert(!readiness_get_night(0, NULL, NULL, NULL, NULL));
}

//! А если удары пошли, пусть и без вариабельности, замер идёт своим сроком:
//! пульс покоя мы ещё можем измерить.
void test_readiness__some_data_keeps_the_measurement_running(void) {
  prv_set_time(JAN_1_2027 + 2 * 60 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  prv_send_bpm(54);

  prv_set_time(JAN_1_2027 + 2 * 60 * 60 + 150);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  cl_assert_equal_i(s_unsubscribe_count, 0);
  cl_assert_equal_i(s_subscribe_count, 1);
}

//! Интервалы, в которых датчик не уверен, в вариабельность не идут: шум в
//! пределах ±200 мс иначе проходил фильтр и завышал оценку.
void test_readiness__poor_quality_intervals_are_ignored(void) {
  prv_set_time(JAN_1_2027 + 2 * 60 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);

  prv_send_bpm(52);
  // Шумные интервалы с большим разбросом — их быть не должно в RMSSD.
  for (int i = 0; i < 60; i++) {
    prv_send_ppi_q((i % 2) ? 900 : 1080, HRMQuality_Poor);
  }
  // Годные — те же, что в спокойной ночи: RMSSD ровно 40.
  for (int i = 0; i < 130; i++) {
    prv_send_ppi((i % 2) ? 1000 : 1040);
  }
  fake_system_task_callbacks_invoke_pending();

  uint16_t rmssd = 0;
  cl_assert(readiness_get_night(0, NULL, &rmssd, NULL, NULL));
  cl_assert_equal_i(rmssd, 40);
}

//! Записанная ночь закрывает сутки: следующие события глубокого сна не
//! заводят пульсометр заново.
void test_readiness__recorded_night_stops_attempts(void) {
  prv_set_time(JAN_1_2027 + 2 * 60 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  prv_good_night(52);
  cl_assert_equal_i(s_subscribe_count, 1);

  prv_set_time(JAN_1_2027 + 3 * 60 * 60);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  prv_sleep_event(ActivitySleepStateRestfulSleep);
  cl_assert_equal_i(s_subscribe_count, 1);
}
