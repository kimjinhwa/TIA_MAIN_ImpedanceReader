#include "dataSync.h"

#include <string.h>

extern "C"
{
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
}

static _cell_value s_cellSnapshot[2][MAX_INSTALLED_CELLS];
static volatile uint8_t s_activeSnapshotIdx = 0;
static volatile uint32_t s_snapshotSeq = 0;
static SemaphoreHandle_t s_systemConfigMutex = nullptr;

void dataSyncInit(void)
{
  if (s_systemConfigMutex == nullptr)
    s_systemConfigMutex = xSemaphoreCreateMutex();
}

void dataSyncPublishCellSnapshot(const _cell_value *src, size_t count)
{
  if (!src)
    return;
  size_t n = count;
  if (n > MAX_INSTALLED_CELLS)
    n = MAX_INSTALLED_CELLS;

  const uint8_t inactive = (uint8_t)(s_activeSnapshotIdx ^ 1u);
  memcpy(s_cellSnapshot[inactive], src, n * sizeof(_cell_value));
  if (n < MAX_INSTALLED_CELLS)
    memset(&s_cellSnapshot[inactive][n], 0, (MAX_INSTALLED_CELLS - n) * sizeof(_cell_value));

  s_activeSnapshotIdx = inactive;
  s_snapshotSeq++;
}

uint32_t dataSyncReadCellSnapshot(_cell_value *dst, size_t maxCount)
{
  if (!dst || maxCount == 0)
    return s_snapshotSeq;
  size_t n = maxCount;
  if (n > MAX_INSTALLED_CELLS)
    n = MAX_INSTALLED_CELLS;

  const uint8_t active = s_activeSnapshotIdx;
  memcpy(dst, s_cellSnapshot[active], n * sizeof(_cell_value));
  return s_snapshotSeq;
}

void dataSyncLockSystemConfig(void)
{
  if (s_systemConfigMutex == nullptr)
    dataSyncInit();
  if (s_systemConfigMutex)
    xSemaphoreTake(s_systemConfigMutex, portMAX_DELAY);
}

void dataSyncUnlockSystemConfig(void)
{
  if (s_systemConfigMutex)
    xSemaphoreGive(s_systemConfigMutex);
}
