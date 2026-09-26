/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/wellbeing/wellbeing.h"

#include "wellbeing_private.h"

#include <pbl/drivers/rtc.h>
#include <pbl/logging/logging.h>
#include "pbl/kernel/compiler.h" // PBL_PACKED
#include "pbl/kernel/mutex.h"
#include "pbl/services/activity/activity.h"
#include "pbl/services/hrm/hrm_manager.h"
#include "pbl/services/hrm/hrm_manager_private.h"
#include "pbl/services/i18n/i18n.h"
#include "pbl/services/notifications/notifications.h"
#include "pbl/services/regular_timer.h"
#include "pbl/services/settings/settings_file.h"
#include "pbl/services/system_task.h"
#include "pbl/services/timeline/attribute.h"
#include "pbl/services/timeline/item.h"
#include "pbl/util/math.h"
#include "pbl/util/uuid.h"
#include "resource/resource_ids.auto.h"
#include "util/time/time.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

PBL_LOG_MODULE_DECLARE(service_wellbeing, CONFIG_SERVICE_WELLBEING_LOG_LEVEL);

#define UUID_WELLBEING_DATA_SOURCE \
  {0x9a, 0x14, 0x77, 0x2b, 0x35, 0xd0, 0x4e, 0x18, 0xbb, 0x62, 0x0c, 0x93, 0x71, 0x5a, 0x2f, 0x40}

#define SETTINGS_FILE_NAME   "wellbeing"
#define SETTINGS_FILE_LEN    512
#define SETTINGS_KEY_HISTORY "readiness"

//! The measurement runs once a night and stops as soon as it has enough beats,
//! so the sensor is on for a couple of minutes rather than until morning.
#define MEASURE_INTERVAL_S       1
#define MEASURE_EXPIRE_S         (10 * 60)
#define MEASURE_TARGET_INTERVALS 120

//! Beat intervals outside this range are not beats: 300 ms is 200 bpm, 2000 ms
//! is 30 bpm. Successive differences above 200 ms are artefacts too.
#define PPI_MIN_MS      300
#define PPI_MAX_MS      2000
#define PPI_MAX_DIFF_MS 200

//! Only measure in the small hours: that is when the wearer has been lying
//! still long enough for a resting reading to mean anything.
#define MEASURE_EARLIEST_HOUR 0
#define MEASURE_LATEST_HOUR   6

//! Сколько раз за ночь можно пробовать, если замер не дал ничего.
//!
//! Событие сна приходит примерно раз в минуту (activity_sessions.c шлёт его на
//! каждое изменение накопленных минут), а ночь, в которой датчик не набрал ни
//! ударов, ни пульса, в историю не записывается — и следующая же минута
//! заводила пульсометр заново. Так плохо сидящий ремешок оборачивался
//! пульсометром, работающим всю ночь: самый прожорливый узел часов, десять
//! минут за попытку, сотня попыток до утра. Три попытки — это полчаса работы
//! датчика в худшем случае и всё те же три шанса поймать спокойный контакт.
#define MEASURE_MAX_ATTEMPTS 3

//! Запас сверх срока подписки, после которого замер считается законченным,
//! даже если пульсометр об этом не сообщил. См. prv_finish_if_overdue.
#define MEASURE_GRACE_S 60

//! Сколько ждать первых данных, прежде чем признать контакт плохим.
//!
//! Пульсометр — самый прожорливый узел часов, и на нашей подписке он горит
//! НЕПРЕРЫВНО: менеджер считает подписчика «пора обслужить», пока
//! `интервал − возраст − раскрутка ≤ 0`, а у нас интервал 1 с против 20 с
//! раскрутки (hrm_manager.c, prv_update_sensor_state). Непрерывность нам и
//! нужна — вариабельность считается по соседним ударам, а датчик, включаемый
//! по кругу, рвёт цепочку.
//!
//! Но у менеджера есть своя защита от плохого контакта: продержав датчик
//! HRM_MAX_UNSERVED_TIME_SEC = 120 с без единого годного показания, он
//! помечает подписчиков обслуженными, чтобы те «выждали свой интервал» вместо
//! того, чтобы держать датчик включённым. Нас эта защита не спасает ровно
//! из-за интервала: выждать секунду — значит стать «пора» снова тут же.
//! Съехавший за ночь ремешок оборачивался десятью минутами горящего датчика
//! на попытку и тридцатью за ночь — при том, что за первые две минуты не
//! пришло ни одного удара.
//!
//! Порог тот же, что у менеджера: если за две минуты непрерывной работы не
//! набралось ни интервалов, ни пульса, следующие восемь ничего не изменят.
#define MEASURE_NO_DATA_S 120

#define HISTORY_NIGHTS READINESS_HISTORY_NIGHTS

typedef struct PBL_PACKED ReadinessNight {
  uint16_t day_id; //!< days since the epoch, so nights sort and compare
  uint16_t rmssd_ms;
  uint8_t resting_bpm;
  uint8_t score;
} ReadinessNight;

typedef struct PBL_PACKED ReadinessHistory {
  //! Newest first.
  ReadinessNight nights[HISTORY_NIGHTS];
  uint16_t reported_day_id;
} ReadinessHistory;

// PBL_MUTEX_DEFINE даёт готовый статический мьютекс — отдельная
// инициализация ему не нужна.
//! Состояние замера. Под ним никогда не зовём менеджер пульсометра: его
//! колбэк держит свою блокировку и ждёт эту.
static PBL_MUTEX_DEFINE(s_mutex);
//! Файл истории. Его читает и приложение на своей задаче, и замер на KernelBG;
//! второй pfs_open того же файла отвечает E_BUSY, и замер записывал ночь поверх
//! «пустой» истории, стирая семь прошлых. Под этим мьютексом только файл.
static PBL_MUTEX_DEFINE(s_file_mutex);

// Measurement in progress.
static HRMSessionRef s_session = HRM_INVALID_SESSION_REF;
static uint16_t s_prev_ppi_ms;
static uint32_t s_sum_sq_diff;
static uint32_t s_intervals;
static uint32_t s_bpm_sum;
static uint32_t s_bpm_count;
static uint16_t s_measuring_day_id;
//! Когда подписка на пульсометр должна была кончиться сама.
static time_t s_measure_deadline_utc;
//! Когда замер начался — чтобы отличить «ещё рано» от «ничего не придёт».
static time_t s_measure_started_utc;

//! За какую ночь считаются попытки и сколько их уже было.
static uint16_t s_attempts_day_id;
static uint8_t s_attempts;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Storage

static uint16_t prv_day_id(time_t utc) {
  return (uint16_t)(time_util_get_midnight_of(utc) / SECONDS_PER_DAY);
}

typedef enum {
  HistoryLoad_Ok,
  //! Истории нет или она в другом формате: начинать с чистого листа законно.
  HistoryLoad_Empty,
  //! Файл не прочитался. Писать поверх нельзя — это стёрло бы настоящую историю.
  HistoryLoad_Error,
} HistoryLoad;

static HistoryLoad prv_load(ReadinessHistory *history) {
  memset(history, 0, sizeof(*history));

  pbl_mutex_lock(&s_file_mutex, PBL_FOREVER);
  HistoryLoad result = HistoryLoad_Error;
  SettingsFile file = {{0}};
  if (settings_file_open(&file, SETTINGS_FILE_NAME, SETTINGS_FILE_LEN) == S_SUCCESS) {
    const int len =
        settings_file_get_len(&file, SETTINGS_KEY_HISTORY, sizeof(SETTINGS_KEY_HISTORY));
    if (len == 0) {
      result = HistoryLoad_Empty;
    } else if (len != (int)sizeof(*history)) {
      // Длина записи и есть версия формата: другая длина — другая раскладка
      // (например, поменялось число ночей). Читать её этой структурой нельзя.
      PBL_LOG_WRN("Readiness: history record is %d bytes, expected %d; starting over", len,
                  (int)sizeof(*history));
      result = HistoryLoad_Empty;
    } else if (settings_file_get(&file, SETTINGS_KEY_HISTORY, sizeof(SETTINGS_KEY_HISTORY), history,
                                 sizeof(*history)) == S_SUCCESS) {
      result = HistoryLoad_Ok;
    }
    settings_file_close(&file);
  }
  pbl_mutex_unlock(&s_file_mutex);
  return result;
}

static bool prv_save(const ReadinessHistory *history) {
  pbl_mutex_lock(&s_file_mutex, PBL_FOREVER);
  bool ok = false;
  SettingsFile file = {{0}};
  if (settings_file_open(&file, SETTINGS_FILE_NAME, SETTINGS_FILE_LEN) == S_SUCCESS) {
    ok = settings_file_set(&file, SETTINGS_KEY_HISTORY, sizeof(SETTINGS_KEY_HISTORY), history,
                           sizeof(*history)) == S_SUCCESS;
    settings_file_close(&file);
  }
  pbl_mutex_unlock(&s_file_mutex);
  if (!ok) {
    PBL_LOG_WRN("Readiness: could not save history");
  }
  return ok;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Scoring

//! Median of the nights before `skip_day_id`, or 0 when there are too few.
static void prv_baseline(const ReadinessHistory *history, uint16_t skip_day_id, uint16_t *rmssd_out,
                         uint8_t *bpm_out) {
  uint16_t rmssd[HISTORY_NIGHTS];
  uint8_t bpm[HISTORY_NIGHTS];
  int n = 0;

  for (int i = 0; i < HISTORY_NIGHTS; i++) {
    const ReadinessNight *night = &history->nights[i];
    // Ночь без пульса покоя в точку отсчёта не годится так же, как ночь без
    // ВСР: ноль в выборке тянул бы медиану вниз, и следующая ночь получала бы
    // вычет за «поднявшийся» пульс, которого не было. Датчик отдаёт ВСР и
    // пульс по отдельности, так что ночь с одним и без другого бывает.
    if (night->day_id == 0 || night->day_id == skip_day_id || night->rmssd_ms == 0 ||
        night->resting_bpm == 0) {
      continue;
    }
    rmssd[n] = night->rmssd_ms;
    bpm[n] = night->resting_bpm;
    n++;
  }

  *rmssd_out = 0;
  *bpm_out = 0;
  // Медиана меньше чем из трёх ночей ничего не значит; до этого честный ответ —
  // что точки отсчёта ещё нет.
  if (n < READINESS_BASELINE_NIGHTS) {
    return;
  }

  for (int i = 1; i < n; i++) {
    for (int j = i; j > 0 && rmssd[j] < rmssd[j - 1]; j--) {
      const uint16_t t = rmssd[j];
      rmssd[j] = rmssd[j - 1];
      rmssd[j - 1] = t;
    }
  }
  for (int i = 1; i < n; i++) {
    for (int j = i; j > 0 && bpm[j] < bpm[j - 1]; j--) {
      const uint8_t t = bpm[j];
      bpm[j] = bpm[j - 1];
      bpm[j - 1] = t;
    }
  }

  *rmssd_out = rmssd[n / 2];
  *bpm_out = bpm[n / 2];
}

//! 50 is "an ordinary night for you". Variability above your own baseline pulls
//! the score up, a raised resting heart rate pulls it down hard, because that
//! is the sign that shows up first when the body is working on something.
static uint8_t prv_score(uint16_t rmssd, uint8_t bpm, uint16_t base_rmssd, uint8_t base_bpm) {
  if (base_rmssd == 0) {
    return 0; // no baseline yet
  }
  // Ночь, в которой датчик не набрал ударов, оценивать не по чему. Без этой
  // проверки ratio_pct обращался в ноль, оценка упиралась в нижний предел, и
  // владелец получал уведомление «1 — поберегите себя» ровно за то, что часы
  // ночью съехали с запястья.
  if (rmssd == 0) {
    return 0;
  }

  const int32_t ratio_pct = ((int32_t)rmssd * 100) / base_rmssd;
  int32_t score = 50 + (ratio_pct - 100) / 2;

  if (base_bpm > 0 && bpm > 0) {
    score -= ((int32_t)bpm - base_bpm) * 3;
  }

  return (uint8_t)CLIP(score, 1, 100);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Reporting

static void prv_push_notification(void *context) {
  const ReadinessNight *night = context;

  char body[96];
  const char *phrase;
  if (night->score >= 70) {
    phrase = i18n_noop("Well recovered");
  } else if (night->score >= 40) {
    phrase = i18n_noop("About as usual");
  } else {
    phrase = i18n_noop("Take it easy today");
  }

  AttributeList attr_list = {0};
  attribute_list_add_resource_id(&attr_list, AttributeIdIconTiny, RESOURCE_ID_HEART_TINY);
  attribute_list_add_cstring(&attr_list, AttributeIdTitle,
                             i18n_get(i18n_noop("Readiness"), &attr_list));
  snprintf(body, sizeof(body), "%" PRIu8 " - %s", night->score, i18n_get(phrase, &attr_list));
  attribute_list_add_cstring(&attr_list, AttributeIdBody, body);
  attribute_list_add_uint8(&attr_list, AttributeIdBgColor, GColorJaegerGreenARGB8);

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

//! Report last night, once, when the wearer is up.
//! @return true when there is nothing left to do today: reported now, or
//! nothing to report. false means try again later.
static bool prv_report_if_due(void) {
  // Замеры идут до шести утра. Короткое пробуждение в три часа ночи — ещё не
  // утро: уведомление будило бы, а ночь могла быть не последней.
  const time_t now = rtc_get_time();
  struct tm local;
  localtime_r(&now, &local);
  if (local.tm_hour < MEASURE_LATEST_HOUR) {
    return false;
  }

  ReadinessHistory history;
  if (prv_load(&history) == HistoryLoad_Error) {
    return false;
  }

  const ReadinessNight *night = &history.nights[0];
  if (night->day_id == 0 || night->score == 0 || history.reported_day_id == night->day_id) {
    return true;
  }

  history.reported_day_id = night->day_id;
  // Не записали отметку — не показываем: иначе то же уведомление приходило бы
  // на каждом следующем пробуждении.
  if (!prv_save(&history)) {
    return false;
  }

  static ReadinessNight s_reported;
  s_reported = *night;
  system_task_add_callback(prv_push_notification, &s_reported);
  return true;
}

//! За какие сутки утренний отчёт уже разобран: дальше в эти сутки файл не
//! читаем.
static uint16_t s_report_done_day_id;

//! Проверить отчёт, если человек проснулся. Состояние сна — из памяти счётчика
//! активности; файл читается, только когда отчёт ещё не разобран.
static void prv_report_check(void) {
  const uint16_t today = prv_day_id(rtc_get_time());
  if (s_report_done_day_id == today) {
    return;
  }
  int32_t sleep_state = ActivitySleepStateUnknown;
  if (!activity_get_metric(ActivityMetricSleepState, 1, &sleep_state) ||
      sleep_state != ActivitySleepStateAwake) {
    return;
  }
  if (prv_report_if_due()) {
    s_report_done_day_id = today;
  }
}

static void prv_report_timer_task(void *unused) {
  prv_report_check();
}

//! Событие сна приходит только при изменении накопленных минут сна: проснулся
//! и лежишь спокойно — событий может не быть до вечера. Поэтому отчёт
//! проверяется ещё и по таймеру. Он ложится на минутный тик часов и отдельно
//! процессор не будит.
static void prv_report_timer_cb(void *unused) {
  system_task_add_callback(prv_report_timer_task, NULL);
}

#define REPORT_CHECK_PERIOD_MIN 15
static RegularTimerInfo s_report_timer = {.cb = prv_report_timer_cb};

////////////////////////////////////////////////////////////////////////////////////////////////////
// Measurement

static void prv_finish(void *unused) {
  uint32_t intervals, sum_sq_diff, bpm_sum, bpm_count;
  uint16_t day_id;
  HRMSessionRef session;

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  {
    // Условие остановки держится истинным, пока подписка жива, а до KernelBG
    // очередь доходит не сразу: за это время успевает прийти ещё удар, и
    // prv_finish встаёт в очередь второй раз. Второй проход записал бы ту же
    // ночь ещё раз, вытеснив настоящую и задвоив её в точке отсчёта. Признак
    // того, что заканчивать уже нечего, — снятая подписка.
    if (s_session == HRM_INVALID_SESSION_REF) {
      pbl_mutex_unlock(&s_mutex);
      return;
    }
    session = s_session;
    s_session = HRM_INVALID_SESSION_REF;

    intervals = s_intervals;
    sum_sq_diff = s_sum_sq_diff;
    bpm_sum = s_bpm_sum;
    bpm_count = s_bpm_count;
    day_id = s_measuring_day_id;
  }
  pbl_mutex_unlock(&s_mutex);
  // Вне мьютекса: менеджер берёт свою блокировку, а колбэк датчика под ней ждёт
  // нашу. Колбэк после этой точки уходит на проверке s_session.
  sys_hrm_manager_unsubscribe(session);

  const uint8_t bpm = (bpm_count > 0) ? (uint8_t)(bpm_sum / bpm_count) : 0;

  // Мало годных ударов — считать по ним вариабельность нельзя: получится шум в
  // одежде числа. Но и выбрасывать ночь целиком неправильно: пульс покоя мы
  // измерили, а главное — по пустой истории не отличить «датчик не добрал» от
  // «сервис вообще не работает». Пишем ночь без ВСР: в точку отсчёта такие
  // ночи не идут (prv_baseline пропускает rmssd == 0), а в приложении видно,
  // что замер был.
  uint16_t rmssd = 0;
  if (intervals >= 30) {
    rmssd = (uint16_t)integer_sqrt(sum_sq_diff / intervals);
  } else {
    PBL_LOG_INFO("Readiness: only %" PRIu32 " usable intervals, no HRV for this night", intervals);
    if (bpm == 0) {
      // Ни ударов, ни вариабельности — записывать нечего.
      return;
    }
  }

  ReadinessHistory history;
  if (prv_load(&history) == HistoryLoad_Error) {
    // Лучше потерять одну ночь, чем записать её поверх нечитанной истории.
    PBL_LOG_WRN("Readiness: history unreadable, night not recorded");
    return;
  }

  uint16_t base_rmssd;
  uint8_t base_bpm;
  prv_baseline(&history, day_id, &base_rmssd, &base_bpm);

  ReadinessNight night = {
    .day_id = day_id,
    .rmssd_ms = rmssd,
    .resting_bpm = bpm,
    .score = prv_score(rmssd, bpm, base_rmssd, base_bpm),
  };

  for (int i = HISTORY_NIGHTS - 1; i > 0; i--) {
    history.nights[i] = history.nights[i - 1];
  }
  history.nights[0] = night;
  if (prv_save(&history)) {
    // Ночь есть — до конца суток ни попыток, ни чтений файла на каждом
    // событии сна.
    s_attempts_day_id = day_id;
    s_attempts = MEASURE_MAX_ATTEMPTS;
  }

  PBL_LOG_INFO("Readiness: RMSSD %" PRIu16 " ms, RHR %" PRIu8 ", score %" PRIu8, rmssd, bpm,
               night.score);
}

static void prv_hrm_cb(PebbleHRMEvent *event, void *context) {
  bool done = false;

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  {
    if (s_session == HRM_INVALID_SESSION_REF) {
      pbl_mutex_unlock(&s_mutex);
      return;
    }

    if (event->event_type == HRMEvent_HRV) {
      const uint16_t ppi = event->hrv.ppi_ms;
      // Датчик сам помечает интервалы, в которых не уверен. Шумные интервалы
      // в пределах ±200 мс проходят фильтр ниже и завышают RMSSD, а с ним и
      // оценку, — поэтому их отбрасываем и рвём цепочку, как на пропуске.
      if (event->hrv.quality >= HRMQuality_Acceptable && ppi >= PPI_MIN_MS && ppi <= PPI_MAX_MS) {
        if (s_prev_ppi_ms != 0) {
          const int32_t diff = (int32_t)ppi - s_prev_ppi_ms;
          if (ABS(diff) <= PPI_MAX_DIFF_MS) {
            s_sum_sq_diff += (uint32_t)(diff * diff);
            s_intervals++;
          }
        }
        s_prev_ppi_ms = ppi;
      } else {
        // A gap in the beats breaks the chain: the next difference would span
        // the gap and would not be a beat-to-beat difference at all.
        s_prev_ppi_ms = 0;
      }
      done = (s_intervals >= MEASURE_TARGET_INTERVALS);
    } else if (event->event_type == HRMEvent_BPM) {
      if (event->bpm.quality >= HRMQuality_Acceptable) {
        s_bpm_sum += event->bpm.bpm;
        s_bpm_count++;
      }
    } else if (event->event_type == HRMEvent_SubscriptionExpiring) {
      done = true;
    }
  }
  pbl_mutex_unlock(&s_mutex);

  if (done) {
    system_task_add_callback(prv_finish, NULL);
  }
}

//! Закончить замер, срок которого давно вышел, а конца так и не пришло.
//!
//! Конец замера мы узнаём от самого пульсометра: удары набрались, или пришло
//! событие «подписка истекает». Второе ненадёжно. Подписчикам KernelBG менеджер
//! шлёт его только из обработчика данных (hrm_manager.c,
//! prv_system_task_hrm_handler), да ещё лишь в последние пять секунд срока. А на
//! плохом контакте данных нет вовсе: датчик, не набравший годного пульса за
//! окно обслуживания, менеджер выключает («deferring subscribers»), и затем
//! молча удаляет подписку по сроку. События мы не получаем, s_session остаётся
//! занятым навсегда, и каждая следующая ночь уходит на первой же проверке
//! «замер уже идёт» — до перезагрузки часов. На экране это «Ещё ночей: 1»,
//! застывшее на трое суток.
//!
//! Поэтому срок мы держим сами. Вызывается на каждом событии сна, то есть в
//! том же KernelBG, что и обычный конец замера; повторный конец отсекает
//! проверка s_session внутри prv_finish.
static void prv_finish_if_overdue(void) {
  if (s_session == HRM_INVALID_SESSION_REF) {
    return;
  }
  const time_t now = rtc_get_time();

  // Контакт плохой: датчик горит две минуты, а не пришло ни удара, ни пульса.
  // Держать его ещё восемь минут незачем — см. MEASURE_NO_DATA_S.
  if (now - s_measure_started_utc >= MEASURE_NO_DATA_S && s_intervals == 0 && s_bpm_count == 0) {
    PBL_LOG_INFO("Readiness: no beats in %d s, bad contact; stopping the HRM early",
                 MEASURE_NO_DATA_S);
    prv_finish(NULL);
    return;
  }

  if (now < s_measure_deadline_utc) {
    return;
  }
  PBL_LOG_WRN("Readiness: HRM subscription ran out without telling us, finishing");
  prv_finish(NULL);
}

//! Всё, что можно решить не трогая состояние замера: время суток, наличие
//! датчика, уже записанная за эти сутки ночь. Файл читается вне s_mutex:
//! держать мьютекс замера на время работы с флешем незачем.
static bool prv_should_measure(uint16_t day_id) {
  if (!sys_hrm_manager_is_hrm_present() || !activity_prefs_heart_rate_is_enabled()) {
    return false;
  }

  const time_t now = rtc_get_time();
  struct tm local;
  localtime_r(&now, &local);
  if (local.tm_hour < MEASURE_EARLIEST_HOUR || local.tm_hour >= MEASURE_LATEST_HOUR) {
    return false;
  }

  ReadinessHistory history;
  if (prv_load(&history) == HistoryLoad_Error) {
    return false;
  }
  if (history.nights[0].day_id == day_id) {
    // Ночь уже есть: больше не пробуем и файл до конца суток не читаем.
    s_attempts_day_id = day_id;
    s_attempts = MEASURE_MAX_ATTEMPTS;
    return false;
  }
  return true;
}

static void prv_start_measurement(void) {
  // Замер уже идёт — уходим сразу. Проверка ниже открывает файл настроек, а
  // события сна приходят часто: пока датчик работает, каждое из них означало
  // бы лишнее обращение к флешу. Читать переменную здесь без мьютекса можно:
  // ошибиться она может только в сторону «идёт, хотя уже кончился», и тогда
  // замер начнётся со следующего события.
  if (s_session != HRM_INVALID_SESSION_REF) {
    return;
  }

  const uint16_t day_id = prv_day_id(rtc_get_time());
  if (day_id != s_attempts_day_id) {
    s_attempts_day_id = day_id;
    s_attempts = 0;
  }
  if (s_attempts >= MEASURE_MAX_ATTEMPTS) {
    return;
  }
  if (!prv_should_measure(day_id)) {
    return;
  }

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  {
    // Замер уже идёт: повторная подписка потеряла бы первую сессию.
    if (s_session != HRM_INVALID_SESSION_REF) {
      pbl_mutex_unlock(&s_mutex);
      return;
    }

    s_prev_ppi_ms = 0;
    s_sum_sq_diff = 0;
    s_intervals = 0;
    s_bpm_sum = 0;
    s_bpm_count = 0;
    s_measuring_day_id = day_id;
    s_measure_started_utc = rtc_get_time();
    s_measure_deadline_utc = s_measure_started_utc + MEASURE_EXPIRE_S + MEASURE_GRACE_S;
    s_attempts++;

    // Замер идёт ночью в фоне, живых показаний никто не смотрит: пусть датчик
    // опустошает очередь реже и тратит меньше батареи.
    s_session = hrm_manager_subscribe_with_callback(
        INSTALL_ID_INVALID, MEASURE_INTERVAL_S, MEASURE_EXPIRE_S, HRMFeature_HRV | HRMFeature_BPM,
        false /*low_latency*/, prv_hrm_cb, NULL);
  }
  pbl_mutex_unlock(&s_mutex);

  if (s_session == HRM_INVALID_SESSION_REF) {
    PBL_LOG_WRN("Readiness: could not subscribe to the HRM");
  } else {
    PBL_LOG_INFO("Readiness: measuring");
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void readiness_handle_sleep_update(void) {
  // До всего остального: и новый замер, и утренний отчёт ждут, пока старый
  // замер будет закончен и записан.
  prv_finish_if_overdue();

  int32_t sleep_state = ActivitySleepStateUnknown;
  if (!activity_get_metric(ActivityMetricSleepState, 1, &sleep_state)) {
    return;
  }

  if (sleep_state == ActivitySleepStateAwake) {
    prv_report_check();
  } else if (sleep_state == ActivitySleepStateRestfulSleep) {
    // Deep sleep, not light: that is when the reading is least polluted by
    // movement and by the wearer being half awake.
    prv_start_measurement();
  }
}

bool readiness_get_night(uint8_t index, uint16_t *day_id, uint16_t *rmssd_ms, uint8_t *resting_bpm,
                         uint8_t *score) {
  if (index >= HISTORY_NIGHTS) {
    return false;
  }

  ReadinessHistory history;
  if (prv_load(&history) != HistoryLoad_Ok) {
    return false;
  }

  const ReadinessNight *night = &history.nights[index];
  if (night->day_id == 0) {
    return false;
  }

  if (day_id)
    *day_id = night->day_id;
  if (rmssd_ms)
    *rmssd_ms = night->rmssd_ms;
  if (resting_bpm)
    *resting_bpm = night->resting_bpm;
  if (score)
    *score = night->score;
  return true;
}

#ifdef CONFIG_QEMU
static uint8_t s_demo_scenario;

static void prv_demo_write(void *unused) {
  static const struct {
    uint16_t rmssd;
    uint8_t bpm;
    uint8_t score;
  } s_nights[] = {
    // Оценки старших ночей — какие угодно правдоподобные: они нужны столбикам.
    {61, 52, 58}, {55, 54, 47}, {66, 50, 64}, {58, 53, 52},
    {63, 51, 61}, {52, 55, 41}, {59, 52, 55}, {60, 52, 50},
  };

  ReadinessHistory history;
  memset(&history, 0, sizeof(history));
  const uint16_t today = prv_day_id(rtc_get_time());

  int count = 0;
  uint8_t latest_score = 0;
  uint16_t latest_rmssd = 0;
  switch ((ReadinessDemoScenario)s_demo_scenario) {
    case ReadinessDemo_Measuring:
      count = 3;
      latest_rmssd = 63;
      break;
    case ReadinessDemo_PulseOnly:
      count = 3;
      latest_rmssd = 0;
      break;
    case ReadinessDemo_Recovered:
      count = HISTORY_NIGHTS;
      latest_rmssd = 74;
      latest_score = 78;
      break;
    case ReadinessDemo_Usual:
      count = HISTORY_NIGHTS;
      latest_rmssd = 60;
      latest_score = 51;
      break;
    case ReadinessDemo_TakeItEasy:
      count = HISTORY_NIGHTS;
      latest_rmssd = 41;
      latest_score = 27;
      break;
    case ReadinessDemo_Clear:
    default:
      count = 0;
      break;
  }

  for (int i = 0; i < count; i++) {
    const bool scored = (count == HISTORY_NIGHTS);
    history.nights[i] = (ReadinessNight){
      .day_id = today - i,
      .rmssd_ms = s_nights[i].rmssd,
      .resting_bpm = s_nights[i].bpm,
      .score = scored ? s_nights[i].score : 0,
    };
  }
  if (count > 0) {
    history.nights[0].rmssd_ms = latest_rmssd;
    history.nights[0].score = latest_score;
  }
  // Отчёт о «последней» ночи уже как бы показан: демо-история не должна
  // присылать уведомление при следующем пробуждении.
  history.reported_day_id = today;
  prv_save(&history);
  PBL_LOG_INFO("Readiness: demo history %" PRIu8 " written", s_demo_scenario);
}

void readiness_qemu_demo_msg_callback(const uint8_t *data, uint32_t len) {
  if (len != 1) {
    return;
  }
  // Обработчики канала эмулятора работают на KernelMain, а файл настроек
  // пишется там же, где пишет ночной замер, — в KernelBG.
  s_demo_scenario = data[0];
  system_task_add_callback(prv_demo_write, NULL);
}
#endif

void readiness_init(void) {
  // История ночей читается из файла настроек при первом обращении, а мьютекс
  // статический. Сбрасываем только состояние замера: на часах при загрузке оно
  // и так нулевое, а модульным тестам без сброса одна зависшая ночь портила
  // все следующие.
  s_session = HRM_INVALID_SESSION_REF;
  s_measure_deadline_utc = 0;
  s_attempts_day_id = 0;
  s_attempts = 0;
  s_report_done_day_id = 0;
  regular_timer_add_multiminute_callback(&s_report_timer, REPORT_CHECK_PERIOD_MIN);
}
