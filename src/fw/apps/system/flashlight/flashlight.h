/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "process_management/pebble_process_md.h"

#define FLASHLIGHT_APP_UUID \
  {0x0d, 0x1a, 0x4f, 0x62, 0x8b, 0x37, 0x4c, 0x21, 0x9e, 0x55, 0x7a, 0x18, 0x63, 0xc4, 0xd0, 0x11}

const PebbleProcessMd *flashlight_app_get_info(void);
