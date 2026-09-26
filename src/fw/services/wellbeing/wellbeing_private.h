/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! Everything here runs on KernelBG: reading activity metrics and the settings
//! file blocks, and health events arrive on KernelMain.

void night_mode_init(void);
void night_mode_handle_sleep_update(void);

void inactivity_init(void);

void readiness_init(void);

//! Снести записи снятого диктофона: приложения нет, а файлы остались.
void wellbeing_cleanup_voice_notes(void);
void readiness_handle_sleep_update(void);
