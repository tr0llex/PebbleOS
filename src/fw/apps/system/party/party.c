/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "party.h"

#ifdef CONFIG_APP_PARTY

#include "applib/app.h"
#include "applib/app_timer.h"
#include "applib/event_service_client.h"
#include "applib/fonts/fonts.h"
#include "applib/graphics/gpath.h"
#include "applib/graphics/graphics.h"
#include "applib/ui/ui.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "pbl/services/i18n/i18n.h"
#include "pbl/services/speaker/limits.h"
#include "pbl/services/speaker/speaker_service.h"
#include "pbl/util/math.h"
#include "pbl/util/size.h"
#include "pbl/util/trig.h"

#include <pbl/logging/logging.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

//! @file party.c
//! Вечеринка: вращающийся куб над звёздным полем, мигание экрана и чиптюн из
//! динамика. Единственное приложение здесь, сделанное не ради пользы.
//!
//! Всё считается целыми числами. Дело не в чистоте: мягкий float тянет за собой
//! библиотеку, которой в прошивке иначе нет, а тут на каждый кадр приходится
//! восемь вершин, две дюжины звёзд и шесть граней.

////////////////////////////////////////////////////////////////////////////////////////////////////
// Картинка

//! 25 кадров в секунду. Ниже — рывками, выше — не успевает отрисоваться.
#define FRAME_MS 40

//! Полуребро куба в единицах модели и параметры камеры. Подобраны так, чтобы
//! диагональ куба (полуребро * √3) не вылезала за 200 px даже углом вперёд.
#define CUBE_HALF 30
#define CAM_DIST  150
#define CAM_FOV   150

#define NUM_STARS 24

//! Кислотная палитра: соседние цвета максимально далеки друг от друга, иначе
//! на вращении грани сливаются в одно пятно.
#define NUM_NEON 8

//! Экран моргает раз в девять кадров — меньше трёх раз в секунду. Полноэкранная
//! смена насыщенного цвета с чёрным чаще трёх раз в секунду опасна для людей с
//! фоточувствительной эпилепсией (WCAG 2.3.1); раньше было шесть кадров, 4,2 Гц.
#define STROBE_PERIOD 9

//! Через сколько пробовать вернуть музыку, если её перебил звук поважнее.
#define MUSIC_RETRY_MS 1000

typedef enum PartyMode {
  PartyModeCube = 0,
  PartyModeTunnel,
  PartyModeBars,
  PartyModeCount
} PartyMode;

typedef struct Star {
  int16_t x;
  int16_t y;
  int16_t z;
} Star;

typedef struct PartyAppData {
  Window window;
  Layer canvas;
  AppTimer *frame_timer;

  uint32_t frame;
  PartyMode mode;

  int32_t angle_x;
  int32_t angle_y;

  Star stars[NUM_STARS];
  uint32_t rand_state;

  //! Дорожки чиптюна. Живут, пока живёт приложение: сервис копирует их себе,
  //! но перезапуск петли берёт их отсюда.
  SpeakerNote *bass;
  SpeakerNote *arp;
  SpeakerNote *lead;
  SpeakerNote *perc;
  uint32_t bass_n;
  uint32_t arp_n;
  uint32_t lead_n;
  uint32_t perc_n;
  uint8_t volume;
  bool muted;

  EventServiceInfo speaker_event;

  char hint[48];
  bool hint_visible;
  AppTimer *hint_timer;
  //! Повтор запуска музыки после вытеснения.
  AppTimer *music_retry_timer;
} PartyAppData;

static const GColor8 s_neon[NUM_NEON] = {
  {.argb = GColorFollyARGB8},   {.argb = GColorVividCeruleanARGB8}, {.argb = GColorYellowARGB8},
  {.argb = GColorMagentaARGB8}, {.argb = GColorGreenARGB8},         {.argb = GColorOrangeARGB8},
  {.argb = GColorCyanARGB8},    {.argb = GColorPurpleARGB8},
};

//! Восемь вершин куба в единицах ±1.
static const int8_t s_cube_v[8][3] = {
  {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
  {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},
};

//! Шесть граней, каждая — четыре вершины по обходу.
static const uint8_t s_cube_f[6][4] = {
  {0, 1, 2, 3}, // перед
  {5, 4, 7, 6}, // зад
  {4, 0, 3, 7}, // лево
  {1, 5, 6, 2}, // право
  {4, 5, 1, 0}, // верх
  {3, 2, 6, 7}, // низ
};

//! Обычный xorshift: нужен разброс, а не качество.
static uint32_t prv_rand(PartyAppData *data) {
  uint32_t x = data->rand_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  data->rand_state = x;
  return x;
}

static void prv_star_respawn(PartyAppData *data, Star *star, bool far_away) {
  star->x = (int16_t)((int32_t)(prv_rand(data) % 400) - 200);
  star->y = (int16_t)((int32_t)(prv_rand(data) % 400) - 200);
  star->z = (int16_t)(far_away ? 400 : (int32_t)(prv_rand(data) % 400) + 20);
}

static void prv_rotate(int32_t x, int32_t y, int32_t z, int32_t ax, int32_t ay, int32_t *ox,
                       int32_t *oy, int32_t *oz) {
  const int32_t sy = sin_lookup(ay);
  const int32_t cy = cos_lookup(ay);
  const int32_t sx = sin_lookup(ax);
  const int32_t cx = cos_lookup(ax);

  // Вокруг Y, затем вокруг X. Множители — ±65535, координаты — ±30, так что
  // произведение с запасом влезает в int32.
  const int32_t x1 = (x * cy - z * sy) / TRIG_MAX_RATIO;
  const int32_t z1 = (x * sy + z * cy) / TRIG_MAX_RATIO;
  const int32_t y2 = (y * cx - z1 * sx) / TRIG_MAX_RATIO;
  const int32_t z2 = (y * sx + z1 * cx) / TRIG_MAX_RATIO;

  *ox = x1;
  *oy = y2;
  *oz = z2;
}

static GPoint prv_project(int32_t x, int32_t y, int32_t z, GPoint center) {
  const int32_t denom = z + CAM_DIST;
  const int32_t d = (denom < 1) ? 1 : denom;
  return GPoint((int16_t)(center.x + (x * CAM_FOV) / d), (int16_t)(center.y + (y * CAM_FOV) / d));
}

static void prv_draw_stars(GContext *ctx, PartyAppData *data, GRect bounds) {
  const GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);

  for (int i = 0; i < NUM_STARS; i++) {
    Star *star = &data->stars[i];
    star->z -= 12;
    if (star->z < 20) {
      prv_star_respawn(data, star, true);
    }

    const GPoint p = prv_project(star->x, star->y, star->z, center);
    if (p.x < 0 || p.y < 0 || p.x >= bounds.size.w || p.y >= bounds.size.h) {
      continue;
    }
    // Ближняя звезда — крупнее и ярче: без этого поле выглядит плоским шумом.
    const int16_t size = (star->z < 120) ? 3 : ((star->z < 240) ? 2 : 1);
    graphics_context_set_fill_color(ctx, s_neon[(i + data->frame / 4) % NUM_NEON]);
    graphics_fill_rect(ctx, &(GRect){{p.x, p.y}, {size, size}});
  }
}

static void prv_draw_cube(GContext *ctx, PartyAppData *data, GRect bounds) {
  const GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);
  GPoint screen[8];
  int32_t depth[8];

  for (int i = 0; i < 8; i++) {
    int32_t x, y, z;
    prv_rotate(s_cube_v[i][0] * CUBE_HALF, s_cube_v[i][1] * CUBE_HALF, s_cube_v[i][2] * CUBE_HALF,
               data->angle_x, data->angle_y, &x, &y, &z);
    screen[i] = prv_project(x, y, z, center);
    depth[i] = z;
  }

  // Художник: сначала дальние грани. Шесть штук — сортировать нечем, кроме
  // как выбором, зато без единого лишнего байта.
  int32_t face_z[6];
  for (int f = 0; f < 6; f++) {
    face_z[f] = 0;
    for (int v = 0; v < 4; v++) {
      face_z[f] += depth[s_cube_f[f][v]];
    }
  }

  bool drawn[6] = {false};
  for (int step = 0; step < 6; step++) {
    int best = -1;
    for (int f = 0; f < 6; f++) {
      if (!drawn[f] && (best < 0 || face_z[f] > face_z[best])) {
        best = f;
      }
    }
    drawn[best] = true;

    GPoint pts[4];
    for (int v = 0; v < 4; v++) {
      pts[v] = screen[s_cube_f[best][v]];
    }
    const GPathInfo info = {.num_points = 4, .points = pts};
    GPath path;
    gpath_init(&path, &info);

    graphics_context_set_fill_color(ctx, s_neon[(best + data->frame / 3) % NUM_NEON]);
    gpath_draw_filled(ctx, &path);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    gpath_draw_outline(ctx, &path);
  }
}

static void prv_draw_tunnel(GContext *ctx, PartyAppData *data, GRect bounds) {
  const GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);

  // Кольца уезжают в глубину: размер задаётся тем же перспективным делением,
  // что и у куба, поэтому движение читается как полёт, а не как пульсация.
  for (int i = 11; i >= 0; i--) {
    const int32_t z = 40 + ((i * 40 + (int32_t)(data->frame * 8)) % 480);
    const int32_t half = (90 * CAM_FOV) / (z + CAM_DIST);
    const int32_t angle = (int32_t)((data->frame * 700 + i * 2000) % TRIG_MAX_ANGLE);

    GPoint pts[4];
    for (int v = 0; v < 4; v++) {
      const int32_t a = (angle + v * (TRIG_MAX_ANGLE / 4)) % TRIG_MAX_ANGLE;
      pts[v] = GPoint((int16_t)(center.x + (half * cos_lookup(a)) / TRIG_MAX_RATIO),
                      (int16_t)(center.y + (half * sin_lookup(a)) / TRIG_MAX_RATIO));
    }
    const GPathInfo info = {.num_points = 4, .points = pts};
    GPath path;
    gpath_init(&path, &info);
    graphics_context_set_stroke_color(ctx, s_neon[(i + data->frame / 2) % NUM_NEON]);
    gpath_draw_outline(ctx, &path);
  }
}

static void prv_draw_bars(GContext *ctx, PartyAppData *data, GRect bounds) {
  // Полосы бегут вниз и меняют цвет по синусу — то же, что делали демо на
  // машинах, у которых на большее не хватало такта.
  const int16_t h = 14;
  for (int16_t y = -h; y < bounds.size.h; y += h) {
    const int32_t a = (int32_t)((y * 900 + data->frame * 2500) % TRIG_MAX_ANGLE);
    const int32_t wobble = (40 * sin_lookup(a)) / TRIG_MAX_RATIO;
    graphics_context_set_fill_color(ctx, s_neon[((y / h) + data->frame / 2 + NUM_NEON) % NUM_NEON]);
    graphics_fill_rect(ctx, &(GRect){{(int16_t)wobble, y}, {bounds.size.w, (int16_t)(h - 2)}});
  }
}

static void prv_canvas_update(Layer *layer, GContext *ctx) {
  PartyAppData *data = app_state_get_user_data();
  const GRect bounds = layer->bounds;

  // Вспышка: раз в шесть кадров фон становится цветным, в остальные — чёрным.
  const bool flash = (data->frame % STROBE_PERIOD) == 0;
  graphics_context_set_fill_color(
      ctx, flash ? s_neon[(data->frame / STROBE_PERIOD) % NUM_NEON] : GColorBlack);
  graphics_fill_rect(ctx, &bounds);

  switch (data->mode) {
    case PartyModeCube:
      prv_draw_stars(ctx, data, bounds);
      prv_draw_cube(ctx, data, bounds);
      break;
    case PartyModeTunnel:
      prv_draw_tunnel(ctx, data, bounds);
      break;
    case PartyModeBars:
      prv_draw_bars(ctx, data, bounds);
      break;
    default:
      break;
  }

  if (data->hint_visible) {
    const GRect box = {{0, (int16_t)(bounds.size.h - 34)}, {bounds.size.w, 30}};
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, &box);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, data->hint, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), box,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Музыка
//
// Четыре монофонические дорожки, которые сервис смешивает: бас, арпеджио,
// мелодия и «ударные». Ровно тот состав, что был у звуковых чипов той эпохи, и
// ровно поэтому получается похоже.

//! 150 ударов в минуту: шестнадцатая — 100 мс, восьмая — 200, четверть — 400.
#define STEP_MS       100
#define BARS          8
#define STEPS_PER_BAR 16

//! Ля минор: i — i — VI — VI — III — III — V — V. Тональность выбрана за то,
//! что на квадратной волне она звучит злее прочих.
static const uint8_t s_chord_root[BARS] = {45, 45, 41, 41, 48, 48, 40, 40};
static const bool s_chord_minor[BARS] = {true, true, false, false, false, false, false, false};

//! Мелодия: полутона от корня аккорда, по восьмой на шаг.
#define LEAD_REST 0xFF
//! Короткое имя только на время таблицы: в ней важен рисунок, а не буквы.
#define R LEAD_REST
static const uint8_t s_lead[BARS][8] = {
  {12, R, 15, R, 19, 15, 12, R}, {12, 15, 19, 22, 19, R, 15, R}, {12, R, 16, R, 19, 16, 12, R},
  {16, 19, 24, R, 19, 16, R, R}, {12, R, 16, 19, 16, R, 12, R},  {19, 16, 12, R, 16, 19, R, R},
  {12, 16, 19, 16, 12, R, R, R}, {19, R, 16, R, 12, R, R, R},
};
#undef R

//! Сумма длительностей дорожки в миллисекундах.
static uint32_t prv_track_ms(const SpeakerNote *notes, uint32_t count) {
  uint32_t total = 0;
  for (uint32_t i = 0; i < count; i++) {
    total += notes[i].duration_ms;
  }
  return total;
}

//! Все дорожки обязаны быть одной длины: сервис играет их параллельно и по
//! кругу, и любая разница накапливается от петли к петле.
static bool prv_tracks_same_length(const PartyAppData *data) {
  const uint32_t want = (uint32_t)BARS * STEPS_PER_BAR * STEP_MS;
  const uint32_t got[4] = {
    prv_track_ms(data->lead, data->lead_n),
    prv_track_ms(data->arp, data->arp_n),
    prv_track_ms(data->bass, data->bass_n),
    prv_track_ms(data->perc, data->perc_n),
  };
  for (int i = 0; i < 4; i++) {
    if (got[i] != want) {
      PBL_LOG_ERR("Party: track %d is %" PRIu32 " ms, expected %" PRIu32, i, got[i], want);
      return false;
    }
  }
  return true;
}

static SpeakerNote *prv_alloc_notes(uint32_t count) {
  SpeakerNote *notes = app_malloc(count * sizeof(SpeakerNote));
  if (notes) {
    memset(notes, 0, count * sizeof(SpeakerNote));
  }
  return notes;
}

static void prv_set(SpeakerNote *n, uint8_t midi, uint8_t wave, uint16_t ms, uint8_t vel) {
  n->midi_note = midi;
  n->waveform = wave;
  n->duration_ms = ms;
  n->velocity = vel;
}

//! Собрать дорожки из аккордов и рисунка. Держать это таблицами в исходнике
//! было бы четыреста строк цифр, в которых первая же опечатка неуловима.
//!
//! Длительность ноты здесь — это её ШАГ, а не то, сколько она звучит: сервис
//! начинает следующую ноту сразу за предыдущей. Поэтому все четыре дорожки
//! обязаны давать одну и ту же сумму, иначе за петлю они разъедутся — у меня
//! бас уходил от арпеджио на 1.28 секунды за проход. Отрывистость делается не
//! укорачиванием ноты, а парой «удар + пауза», как у ударных ниже.
//!
//! Проверка суммы стоит прямо в коде: молчаливое расхождение слышно не сразу,
//! а понять, что именно не так, на слух почти нельзя.
static bool prv_build_song(PartyAppData *data) {
  data->bass_n = BARS * 8;            // восьмыми
  data->arp_n = BARS * STEPS_PER_BAR; // шестнадцатыми
  data->lead_n = BARS * 8;            // восьмыми
  data->perc_n = BARS * 8 * 2;        // удар + пауза на каждую восьмую

  data->bass = prv_alloc_notes(data->bass_n);
  data->arp = prv_alloc_notes(data->arp_n);
  data->lead = prv_alloc_notes(data->lead_n);
  data->perc = prv_alloc_notes(data->perc_n);
  if (!data->bass || !data->arp || !data->lead || !data->perc) {
    return false;
  }

  for (int bar = 0; bar < BARS; bar++) {
    const uint8_t root = s_chord_root[bar];
    const uint8_t third = (uint8_t)(root + (s_chord_minor[bar] ? 3 : 4));
    const uint8_t fifth = (uint8_t)(root + 7);
    const uint8_t tones[4] = {root, third, fifth, (uint8_t)(root + 12)};

    for (int i = 0; i < 8; i++) {
      // Бас: корень, а на четвёртой и восьмой восьмой — октавой выше. Ход,
      // на котором держится вся эта музыка.
      const uint8_t low = (i == 3 || i == 7) ? (uint8_t)(root + 12) : root;
      prv_set(&data->bass[bar * 8 + i], low, SpeakerWaveformTriangle, STEP_MS * 2, 110);

      // Мелодия: тон от корня, паузы остаются паузами.
      const uint8_t step = s_lead[bar][i];
      prv_set(&data->lead[bar * 8 + i], (step == LEAD_REST) ? 0 : (uint8_t)(root + step),
              SpeakerWaveformSquare, STEP_MS * 2, 95);

      // Ударные: бочка на первую и третью долю, закрытый хай-хэт на каждую
      // вторую восьмую. Шумовой волны в синтезаторе нет, поэтому хэт — очень
      // короткая пила наверху, а бочка — такая же внизу. Остаток восьмой
      // добирается паузой, чтобы шаг остался прежним.
      const int at = (bar * 8 + i) * 2;
      if (i % 4 == 0) {
        prv_set(&data->perc[at], 33, SpeakerWaveformSawtooth, 60, 120);
        prv_set(&data->perc[at + 1], 0, SpeakerWaveformSawtooth, STEP_MS * 2 - 60, 0);
      } else if (i % 2 == 1) {
        prv_set(&data->perc[at], 90, SpeakerWaveformSawtooth, 30, 45);
        prv_set(&data->perc[at + 1], 0, SpeakerWaveformSawtooth, STEP_MS * 2 - 30, 0);
      } else {
        prv_set(&data->perc[at], 0, SpeakerWaveformSawtooth, STEP_MS, 0);
        prv_set(&data->perc[at + 1], 0, SpeakerWaveformSawtooth, STEP_MS, 0);
      }
    }

    for (int i = 0; i < STEPS_PER_BAR; i++) {
      // Арпеджио двумя октавами выше и с разворотом на второй половине такта:
      // ровный подъём за восемь тактов надоедает раньше, чем кончается петля.
      const int idx = (i < 8) ? (i % 4) : (3 - (i % 4));
      prv_set(&data->arp[bar * STEPS_PER_BAR + i], (uint8_t)(tones[idx] + 24),
              SpeakerWaveformSquare, STEP_MS, 60);
    }
  }

  return prv_tracks_same_length(data);
}

static void prv_play(PartyAppData *data) {
  if (data->muted) {
    return;
  }
  const SpeakerTrack tracks[4] = {
    {.notes = data->lead, .num_notes = data->lead_n, .sample = NULL},
    {.notes = data->arp, .num_notes = data->arp_n, .sample = NULL},
    {.notes = data->bass, .num_notes = data->bass_n, .sample = NULL},
    {.notes = data->perc, .num_notes = data->perc_n, .sample = NULL},
  };
  // Будильник и другие системные звуки забирают себе событие «кончилось»
  // (alarm_popup.c регистрирует его для KernelMain), поэтому просим его заново
  // перед каждым запуском — иначе после будильника петля не продолжится.
  speaker_service_register_finish(PebbleTask_App);
  // Хозяина ставим только после успешного запуска: если бы звук поважнее не
  // пустил нас, мы записали бы в хозяева его звук и оборвали бы его при выходе.
  if (speaker_service_play_tracks(tracks, ARRAY_LENGTH(tracks), SpeakerPriorityApp, data->volume)) {
    speaker_service_set_owner_task(PebbleTask_App);
  }
}

static void prv_music_retry(void *context);

//! Завести музыку, если динамик свободен, иначе попробовать позже.
static void prv_music_try(PartyAppData *data) {
  if (speaker_service_get_state() != SpeakerStateIdle) {
    // Чужой звук ещё играет — ждём, а не перебиваем его.
    if (!data->music_retry_timer) {
      data->music_retry_timer = app_timer_register(MUSIC_RETRY_MS, prv_music_retry, data);
    }
    return;
  }
  prv_play(data);
}

static void prv_music_retry(void *context) {
  PartyAppData *data = context;
  data->music_retry_timer = NULL; // этот таймер уже сработал
  prv_music_try(data);
}

static void prv_speaker_event(PebbleEvent *event, void *context) {
  PartyAppData *data = app_state_get_user_data();
  switch ((SpeakerFinishReason)event->speaker.finish_reason) {
    case SpeakerFinishReasonDone:
      // Петля: дорожки кончились, заводим их заново. Пауза между заходами —
      // время одного сообщения, на слух её нет.
      prv_music_try(data);
      break;
    case SpeakerFinishReasonPreempted:
    case SpeakerFinishReasonError:
      // Перебил звук поважнее (уведомление) или сбой: вернуться, когда динамик
      // освободится, а не лезть в чужой звук сразу.
      if (!data->music_retry_timer) {
        data->music_retry_timer = app_timer_register(MUSIC_RETRY_MS, prv_music_retry, data);
      }
      break;
    case SpeakerFinishReasonStopped:
    default:
      // Остановили мы сами (звук выключен или выходим) — продолжать нечего.
      break;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Кадры и кнопки

static void prv_hint_timeout(void *context) {
  PartyAppData *data = context;
  data->hint_timer = NULL;
  data->hint_visible = false;
}

static void prv_show_hint(PartyAppData *data, const char *text) {
  strncpy(data->hint, text, sizeof(data->hint) - 1);
  data->hint[sizeof(data->hint) - 1] = '\0';
  data->hint_visible = true;
  if (data->hint_timer) {
    app_timer_reschedule(data->hint_timer, 1500);
  } else {
    data->hint_timer = app_timer_register(1500, prv_hint_timeout, data);
  }
}

static void prv_frame(void *context) {
  PartyAppData *data = context;
  data->frame++;
  data->angle_y = (data->angle_y + 1400) % TRIG_MAX_ANGLE;
  data->angle_x = (data->angle_x + 900) % TRIG_MAX_ANGLE;
  layer_mark_dirty(&data->canvas);
  data->frame_timer = app_timer_register(FRAME_MS, prv_frame, data);
}

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  PartyAppData *data = app_state_get_user_data();
  data->mode = (PartyMode)((data->mode + 1) % PartyModeCount);
}

static void prv_volume_step(PartyAppData *data, int delta) {
  int vol = data->volume + delta;
  data->volume = (uint8_t)CLIP(vol, 0, 100);
  speaker_service_set_volume(data->volume);

  char text[32];
  snprintf(text, sizeof(text), "%s %d%%", i18n_get("Volume", data), data->volume);
  prv_show_hint(data, text);
}

static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  prv_volume_step(app_state_get_user_data(), 10);
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  prv_volume_step(app_state_get_user_data(), -10);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Окно

static void prv_window_load(Window *window) {
  PartyAppData *data = window_get_user_data(window);

  Layer *root = window_get_root_layer(window);
  layer_init(&data->canvas, &root->bounds);
  layer_set_update_proc(&data->canvas, prv_canvas_update);
  layer_add_child(root, &data->canvas);

  for (int i = 0; i < NUM_STARS; i++) {
    prv_star_respawn(data, &data->stars[i], false);
  }

  data->frame_timer = app_timer_register(FRAME_MS, prv_frame, data);

  if (data->muted) {
    // Молча играть в тишину и делать вид, что всё идёт по плану, — худшее из
    // возможного: человек решит, что сломался динамик.
    prv_show_hint(data, i18n_get("Sound is muted", data));
  } else {
    prv_play(data);
  }
}

static void prv_window_unload(Window *window) {
  PartyAppData *data = window_get_user_data(window);

  if (data->frame_timer) {
    app_timer_cancel(data->frame_timer);
    data->frame_timer = NULL;
  }
  if (data->hint_timer) {
    app_timer_cancel(data->hint_timer);
    data->hint_timer = NULL;
  }
  if (data->music_retry_timer) {
    app_timer_cancel(data->music_retry_timer);
    data->music_retry_timer = NULL;
  }

  event_service_client_unsubscribe(&data->speaker_event);
  speaker_service_stop_for_task(PebbleTask_App);

  layer_deinit(&data->canvas);
  i18n_free_all(data);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Приложение

static void prv_main(void) {
  PartyAppData *data = app_zalloc_check(sizeof(PartyAppData));
  app_state_set_user_data(data);

  data->rand_state = 0x50415254; // «PART»: лишь бы не ноль, xorshift из нуля не выходит
  // Половина громкости: приложение и так шумное, а первое впечатление не
  // должно быть "скорее выключить". Кнопками вверх/вниз добирается остальное.
  data->volume = 50;
  data->muted = speaker_service_is_muted();

  if (!prv_build_song(data)) {
    // Без музыки вечеринка всё ещё вечеринка: картинка на месте, звука нет.
    data->muted = true;
  }

  data->speaker_event = (EventServiceInfo){
    .type = PEBBLE_SPEAKER_EVENT,
    .handler = prv_speaker_event,
  };
  event_service_client_subscribe(&data->speaker_event);
  speaker_service_register_finish(PebbleTask_App);

  window_init(&data->window, WINDOW_NAME("Party"));
  window_set_user_data(&data->window, data);
  window_set_click_config_provider(&data->window, prv_click_config_provider);
  window_set_window_handlers(&data->window, &(WindowHandlers){
                                              .load = prv_window_load,
                                              .unload = prv_window_unload,
                                            });
  app_window_stack_push(&data->window, true /* animated */);

  app_event_loop();

  app_free(data->bass);
  app_free(data->arp);
  app_free(data->lead);
  app_free(data->perc);
  app_free(data);
}

const PebbleProcessMd *party_app_get_info(void) {
  static const PebbleProcessMdSystem s_party_app_info = {
    .common =
        {
          .main_func = &prv_main,
          .uuid = PARTY_APP_UUID,
        },
    .name = i18n_noop("Party"),
    .icon_resource_id = RESOURCE_ID_PARTY_TINY,
  };
  return (const PebbleProcessMd *)&s_party_app_info;
}

#endif // CONFIG_APP_PARTY
