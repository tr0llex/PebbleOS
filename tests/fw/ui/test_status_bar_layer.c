/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "applib/graphics/utf8.h"
#include "applib/ui/status_bar_layer.h"
#include "pbl/util/list.h"
#include "resource/resource_ids.auto.h"
#include "resource/resource.h"

#include "clar.h"

// Fakes
////////////////////////////////////
#include "fake_fonts.h"

// Stubs
////////////////////////////////////
#include "stubs_app_state.h"
#include "stubs_app_timer.h"
#include "stubs_applib_resource.h"
#include "stubs_compiled_with_legacy2_sdk.h"
#include "stubs_event_service_client.h"
#include "stubs_heap.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_print.h"
#include "stubs_process_manager.h"
#include "stubs_resources.h"
#include "stubs_syscalls.h"
#include "stubs_ui_window.h"
#include "stubs_unobstructed_area.h"
#include "stubs_window_stack.h"

// Stubs
////////////////////////////////////
GContext *graphics_context_get_current_context(void) {
  return NULL;
}

// Setup
////////////////////////////////////

ResourceCallbackHandle resource_watch(ResAppNum app_num, uint32_t resource_id,
                                      ResourceChangedCallback callback, void *data) {
  return (ResourceCallbackHandle){0};
}

// Helpers
////////////////////////////////////

#define cl_assert_status_bar_height(status_bar)                           \
  do {                                                                    \
    cl_assert(status_bar.layer.frame.size.h == STATUS_BAR_LAYER_HEIGHT);  \
    cl_assert(status_bar.layer.bounds.size.h == STATUS_BAR_LAYER_HEIGHT); \
  } while (0);

// Tests
////////////////////////////////////

//! The height of the status bar should always be locked to STATUS_BAR_LAYER_HEIGHT.
//! Make sure that after marking dirty, it is always reset to STATUS_BAR_LAYER_HEIGHT.
void test_status_bar_layer__modify_height(void) {
  StatusBarLayer status_bar;
  status_bar_layer_init(&status_bar);

  cl_assert_status_bar_height(status_bar);

  GRect frame = status_bar.layer.frame;
  GRect bounds = status_bar.layer.bounds;

  frame.size.h = STATUS_BAR_LAYER_HEIGHT - 5;
  layer_set_frame(&status_bar.layer, &frame);
  cl_assert_status_bar_height(status_bar);

  bounds.size.h = STATUS_BAR_LAYER_HEIGHT + 5;
  layer_set_bounds(&status_bar.layer, &bounds);
  cl_assert_status_bar_height(status_bar);
}

//! Switching to the large-bold clock mode must grow the bar to the per-platform
//! large height, and that height must survive subsequent frame/bounds pokes
//! (the property-changed proc re-derives height from the mode).
void test_status_bar_layer__large_bold_height(void) {
  StatusBarLayer status_bar;
  status_bar_layer_init(&status_bar);
  cl_assert_status_bar_height(status_bar); // default height

  status_bar_layer_set_mode(&status_bar, StatusBarLayerModeClockLargeBold);
  cl_assert(status_bar.layer.frame.size.h == STATUS_BAR_LAYER_LARGE_BOLD_HEIGHT);
  cl_assert(status_bar.layer.bounds.size.h == STATUS_BAR_LAYER_LARGE_BOLD_HEIGHT);

  GRect frame = status_bar.layer.frame;
  frame.size.h = STATUS_BAR_LAYER_LARGE_BOLD_HEIGHT - 5;
  layer_set_frame(&status_bar.layer, &frame);
  cl_assert(status_bar.layer.frame.size.h == STATUS_BAR_LAYER_LARGE_BOLD_HEIGHT);
  cl_assert(status_bar.layer.bounds.size.h == STATUS_BAR_LAYER_LARGE_BOLD_HEIGHT);

  GRect bounds = status_bar.layer.bounds;
  bounds.size.h = STATUS_BAR_LAYER_LARGE_BOLD_HEIGHT + 5;
  layer_set_bounds(&status_bar.layer, &bounds);
  cl_assert(status_bar.layer.bounds.size.h == STATUS_BAR_LAYER_LARGE_BOLD_HEIGHT);

  status_bar_layer_set_mode(&status_bar, StatusBarLayerModeClock);
  cl_assert_status_bar_height(status_bar); // back to default
}

//! A title longer than the buffer must be cut between characters and stay terminated, or the
//! renderer rejects the whole string and the title disappears.
void test_status_bar_layer__long_multibyte_title(void) {
  StatusBarLayer status_bar;
  status_bar_layer_init(&status_bar);

  // Twelve two-byte characters (U+0416), longer than TITLE_TEXT_BUFFER_SIZE
  const char *title =
      "\xd0\x96\xd0\x96\xd0\x96\xd0\x96\xd0\x96\xd0\x96"
      "\xd0\x96\xd0\x96\xd0\x96\xd0\x96\xd0\x96\xd0\x96";
  cl_assert(strlen(title) >= TITLE_TEXT_BUFFER_SIZE);
  status_bar_layer_set_title(&status_bar, title, false, false);

  const char *buffer = status_bar.config.title_text_buffer;
  cl_assert(strnlen(buffer, TITLE_TEXT_BUFFER_SIZE) < TITLE_TEXT_BUFFER_SIZE);
  cl_assert(utf8_is_valid_string(buffer));
  const size_t len = strlen(buffer);
  cl_assert(len >= strlen(UTF8_ELLIPSIS_STRING));
  cl_assert_equal_s(&buffer[len - strlen(UTF8_ELLIPSIS_STRING)], UTF8_ELLIPSIS_STRING);
}

//! A title that fits is copied unchanged.
void test_status_bar_layer__short_title(void) {
  StatusBarLayer status_bar;
  status_bar_layer_init(&status_bar);

  status_bar_layer_set_title(&status_bar, "Settings", false, false);
  cl_assert_equal_s(status_bar.config.title_text_buffer, "Settings");
}
