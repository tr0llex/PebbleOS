/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! @file stopwatch.h
//! Stopwatch and countdown timer.
//!
//! The state lives in the kernel rather than in the app so that both keep
//! running once the wearer leaves the app — a countdown that dies when you go
//! look at the time is not a countdown. The app is a view onto this service.
//!
//! Elapsed time is measured in RTC ticks, so setting the clock or crossing a
//! DST boundary does not move a running stopwatch.

//! How many lap splits are kept. Older laps are dropped.
#define STOPWATCH_MAX_LAPS 10

//! Longest countdown that can be set, in seconds.
#define STOPWATCH_TIMER_MAX_S (24 * 60 * 60)

void stopwatch_service_init(void);

////////////////////////////////////////////////////////////////////////////////////////////////////
// Stopwatch

//! Start, or resume after a pause. No-op if already running.
void stopwatch_start(void);

//! Stop counting, keeping the elapsed time. No-op if not running.
void stopwatch_pause(void);

//! Stop and clear the elapsed time and every lap.
void stopwatch_reset(void);

bool stopwatch_is_running(void);

//! Elapsed time in milliseconds, whether running or paused.
uint32_t stopwatch_get_elapsed_ms(void);

//! Record a lap split. No-op if not running.
void stopwatch_lap(void);

//! Number of laps recorded, at most STOPWATCH_MAX_LAPS.
uint8_t stopwatch_get_lap_count(void);

//! Laps taken since the last reset, including the ones no longer kept. This is
//! what numbers a lap: past the cap the kept count stops growing.
uint16_t stopwatch_get_lap_total(void);

//! Duration of a lap in milliseconds. Index 0 is the most recent lap.
//! @return 0 if there is no such lap
uint32_t stopwatch_get_lap_ms(uint8_t index);

////////////////////////////////////////////////////////////////////////////////////////////////////
// Countdown timer

//! Start a countdown. Replaces any countdown already set.
//! @param duration_s 1..STOPWATCH_TIMER_MAX_S
void stopwatch_timer_start(uint32_t duration_s);

//! True while a countdown is set, whether running or paused.
bool stopwatch_timer_is_set(void);

bool stopwatch_timer_is_running(void);

void stopwatch_timer_pause(void);

void stopwatch_timer_resume(void);

//! Clear the countdown without alerting.
void stopwatch_timer_cancel(void);

//! Time left in milliseconds, 0 once it has expired.
uint32_t stopwatch_timer_get_remaining_ms(void);

//! The duration the countdown was started with, for restarting it.
uint32_t stopwatch_timer_get_duration_s(void);
