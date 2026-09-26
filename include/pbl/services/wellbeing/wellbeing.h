/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/events.h"

#include <stdbool.h>
#include <stdint.h>

//! @file wellbeing.h
//! Behaviours that act on the wearer's behalf from health data: dimming the
//! watch once they are asleep, nudging them when they have not moved for an
//! hour, and reading overnight heart-rate variability to say how recovered they
//! are. All three are opt-out, and all three stay quiet when there is any doubt
//! about the state they key on.

void wellbeing_service_init(void);

//! Fed from the shell event loop, which is where health events are dispatched.
void wellbeing_handle_health_event(const PebbleHealthEvent *event);

//! Re-evaluate night mode after its preference changed. Safe from any task: the
//! work is deferred to KernelBG, where the rest of the service runs.
void night_mode_handle_prefs_changed(void);

//! Start or stop the step polling after the move reminder preference changed.
//! Off means off: with the reminder disabled nothing should be waking the
//! processor on its behalf. Safe from any task, deferred to KernelBG.
void inactivity_handle_prefs_changed(void);

//! Called once the shell has read the stored preferences. The service starts
//! before that, and loading preferences does not run their change handlers, so
//! without this a reminder switched on stayed dead after every reboot.
void wellbeing_handle_prefs_loaded(void);

//! True while the backlight is being held down because the wearer is asleep.
bool night_mode_is_active(void);

//! Read a stored night, newest at index 0. Any output pointer may be NULL.
//! @return false when there is no night at that index
bool readiness_get_night(uint8_t index, uint16_t *day_id, uint16_t *rmssd_ms, uint8_t *resting_bpm,
                         uint8_t *score);

//! How many nights readiness_get_night() can return.
#define READINESS_HISTORY_NIGHTS 8

#ifdef CONFIG_QEMU
//! Готовые истории ночей для «Готовности» в эмуляторе.
//!
//! Историю пишет только ночной замер пульсометра, а пульсометра, отдающего
//! интервалы между ударами, в эмуляторе нет. Без этого приложение там видно
//! лишь в двух состояниях из шести, и экран с оценкой не проверить ничем,
//! кроме ночи на руке. Код собирается только для эмулятора (CONFIG_QEMU).
typedef enum {
  ReadinessDemo_Clear = 0,      //!< истории нет
  ReadinessDemo_Measuring = 1,  //!< три ночи с ВСР, у последней ещё нет оценки
  ReadinessDemo_PulseOnly = 2,  //!< последняя ночь без ВСР, только пульс покоя
  ReadinessDemo_Recovered = 3,  //!< восемь ночей, последняя — хорошее восстановление
  ReadinessDemo_Usual = 4,      //!< восемь ночей, последняя — как обычно
  ReadinessDemo_TakeItEasy = 5, //!< восемь ночей, последняя — побереги себя
} ReadinessDemoScenario;

//! Обработчик сообщения стенда QemuProtocol_WellbeingDemo (qemu_serial.c).
void readiness_qemu_demo_msg_callback(const uint8_t *data, uint32_t len);
#endif

//! Сколько прошлых ночей нужно, чтобы у оценки появилась точка отсчёта. До
//! этого замеры уже идут и их видно в приложении, но сравнивать их не с чем,
//! поэтому оценки нет. Медиана меньше чем из трёх ночей ничего не значит.
#define READINESS_BASELINE_NIGHTS 3
