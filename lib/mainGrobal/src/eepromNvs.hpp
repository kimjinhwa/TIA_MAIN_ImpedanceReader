#pragma once

#include "mainGrobal.h"
#include <EEPROM.h>

inline bool eepromNvsBlockLooksValid(void)
{
  const size_t tail = (size_t)EEPROM_NV_TAIL_BYTE_OFFSET;
  return EEPROM.read(0) == EEPROM_NV_MAGIC_HEAD && EEPROM.read(tail) == EEPROM_NV_MAGIC_TAIL;
}

inline void eepromNvsWriteBlock(const nvsSystemSet *cfg)
{
  const size_t tail = (size_t)EEPROM_NV_TAIL_BYTE_OFFSET;
  EEPROM.writeByte(0, EEPROM_NV_MAGIC_HEAD);
  EEPROM.writeBytes(1, (const uint8_t *)cfg, sizeof(nvsSystemSet));
  EEPROM.writeByte(tail, EEPROM_NV_MAGIC_TAIL);
}

/** HEAD·TAIL을 깨뜨려 블록을 무효화. 재부팅 후 readnWriteEEProm()이 공장 기본값으로 다시 채움. */
inline void eepromNvsInvalidateBlock(void)
{
  const size_t tail = (size_t)EEPROM_NV_TAIL_BYTE_OFFSET;
  EEPROM.writeByte(0, 0x00);
  EEPROM.writeByte(tail, 0x00);
}
