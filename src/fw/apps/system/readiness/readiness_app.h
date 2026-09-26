/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "process_management/pebble_process_md.h"

#define READINESS_APP_UUID \
  {0x35, 0xc8, 0x0a, 0x71, 0x4d, 0x92, 0x46, 0x0e, 0x83, 0x5b, 0x2f, 0x61, 0xa9, 0x07, 0x33, 0x04}

const PebbleProcessMd *readiness_app_get_info(void);
