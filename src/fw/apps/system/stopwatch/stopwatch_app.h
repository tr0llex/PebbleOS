/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "process_management/pebble_process_md.h"

#define STOPWATCH_APP_UUID \
  {0x7a, 0x41, 0x2e, 0x93, 0x64, 0x0f, 0x4b, 0x8c, 0x91, 0x2a, 0xd6, 0x37, 0x05, 0xbe, 0x11, 0x02}

const PebbleProcessMd *stopwatch_app_get_info(void);
