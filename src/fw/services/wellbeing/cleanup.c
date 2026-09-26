/* SPDX-FileCopyrightText: 2026 pebble-app1 */
/* SPDX-License-Identifier: Apache-2.0 */

#include "wellbeing_private.h"

#include <pbl/logging/logging.h>
#include "pbl/services/filesystem/pfs.h"
#include "pbl/services/settings/settings_file.h"

#include <stdio.h>

//! Снос того, что осталось от снятого диктофона.
//!
//! Приложение убрано, а его записи — нет: они лежат в файловой системе часов и
//! сами оттуда не денутся. Восемь заметок по минуте — это почти восемь
//! мегабайт из двадцати шести, и удалить их теперь нечем, кроме как отсюда.
//!
//! Зовётся при каждом запуске: удаление несуществующего файла ничего не стоит,
//! а признак «уже чистили» пришлось бы где-то хранить, и он пережил бы саму
//! надобность. Эту функцию можно убрать через несколько выпусков, когда ни на
//! одних часах старых записей не останется.
#define VNOTE_PREFIX   "vnote"
#define VNOTE_SLOTS    8
#define VNOTE_SETTINGS "voicenotes"

void wellbeing_cleanup_voice_notes(void) {
  for (int slot = 0; slot < VNOTE_SLOTS; slot++) {
    char name[12];
    snprintf(name, sizeof(name), VNOTE_PREFIX "%d", slot);
    if (pfs_remove(name) == S_SUCCESS) {
      PBL_LOG_INFO("Removed a leftover voice note: %s", name);
    }
  }
  pfs_remove(VNOTE_SETTINGS);
}
