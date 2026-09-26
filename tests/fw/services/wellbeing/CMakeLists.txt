# SPDX-FileCopyrightText: 2026 pebble-app1
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_readiness
  SOURCES
    src/fw/services/wellbeing/readiness.c
    src/fw/util/time/time.c
    src/fw/util/time/mktime.c
    tests/stubs/stubs_i18n.c
    tests/fakes/fake_rtc.c
    tests/fakes/fake_settings_file.c
    src/fw/util/crc8.c
  DEFINES DUMA_DISABLED CONFIG_HRM CONFIG_SERVICE_WELLBEING_LOG_LEVEL=0
  OVERRIDES dummy_board
)
