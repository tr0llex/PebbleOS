/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/wellbeing/wellbeing.h"

#include "wellbeing_private.h"

#include "pbl/services/system_task.h"
#include "applib/health_service.h"

static void prv_handle_sleep_update(void *unused) {
  night_mode_handle_sleep_update();
}

void wellbeing_service_init(void) {
  wellbeing_cleanup_voice_notes();
  night_mode_init();
  inactivity_init();
}

void wellbeing_handle_prefs_loaded(void) {
  night_mode_handle_prefs_changed();
  inactivity_handle_prefs_changed();
}

void wellbeing_handle_health_event(const PebbleHealthEvent *event) {
  // Health events are dispatched on KernelMain; everything these modules do
  // reads the activity service or the filesystem, so none of it belongs here.
  if (event->type == HealthEventSleepUpdate) {
    system_task_add_callback(prv_handle_sleep_update, NULL);
  }
}
