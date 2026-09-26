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
