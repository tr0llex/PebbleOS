/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "process_management/pebble_process_md.h"

#define PARTY_APP_UUID \
  {0x7d, 0x41, 0x9c, 0x26, 0xb8, 0x0e, 0x4f, 0x33, 0x9a, 0x67, 0x2e, 0xd5, 0x10, 0x8b, 0x77, 0x05}

const PebbleProcessMd *party_app_get_info(void);
