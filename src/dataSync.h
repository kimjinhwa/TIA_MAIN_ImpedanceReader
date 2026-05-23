#ifndef DATA_SYNC_H
#define DATA_SYNC_H

#include <stddef.h>
#include <stdint.h>

#include "mainGrobal.h"

void dataSyncInit(void);

/* 측정값 스냅샷(더블버퍼) */
void dataSyncPublishCellSnapshot(const _cell_value *src, size_t count);
uint32_t dataSyncReadCellSnapshot(_cell_value *dst, size_t maxCount);

/* 설정값 보호 mutex */
void dataSyncLockSystemConfig(void);
void dataSyncUnlockSystemConfig(void);

#endif
