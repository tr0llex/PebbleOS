/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "process_management/pebble_process_md.h"

#define COMPASS_APP_UUID \
  {0x2c, 0x6b, 0x14, 0xd0, 0x9f, 0x3a, 0x4e, 0x71, 0xb8, 0x2d, 0x51, 0xc0, 0x7a, 0x44, 0x18, 0x01}

const PebbleProcessMd *compass_app_get_info(void);
