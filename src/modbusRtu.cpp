#include <Arduino.h>
#include <EEPROM.h>
#include <math.h>
#include <Ads1220.h>
#include "modbusRtu.h"
#include "mainGrobal.h"
#include "dataSync.h"
#include "eepromNvs.hpp"
#include "../Version.h"
#include <ModbusClientRTU.h>

extern float bootRcalVerifyRealGet(void);
extern float bootRcalVerifyImageGet(void);
extern float bootRcalVerifyMagnitudeGet(void);
extern void AD5940_UseStoredRcalFromEeprom(void);

extern uint8_t get485Address(void);

char strErrorMessage[40];

uint16_t modbusReg50BaseImpProgress = 0;

/** Modbus주소.md — FC04 최대 셀 수 */
#define MODBUS_MAX_CELLS 16u                      // FC04에서 처리하는 최대 셀 수
#define MODBUS_REG_BASE_IMP_PROGRESS 50u          // FC03/FC06: 기준 내부저항 스캔 진행 상태 레지스터
#define MODBUS_REG_IMP_READ_MAX 15u               // FC03/FC06: 내부저항 최대 읽기 횟수 레지스터
#define MODBUS_REG_IMP_STABLE_WINDOW 16u          // FC03/FC06: 내부저항 안정 판단 윈도우 레지스터
#define MODBUS_REG_IMP_EEPROM_CHANGE_PERCENT 17u  // FC03/FC06: EEPROM 갱신 임계치(%) 레지스터
#define MODBUS_REG_IMP_PERIOD_MIN 18u             // FC03/FC06: 내부저항 측정 주기(분) 레지스터
#define MODBUS_REG_RCAL_BOOT_REAL_HI 19u          // FC03: 부팅 RCAL Real (int32 milli, HI word)
#define MODBUS_REG_RCAL_BOOT_REAL_LO 20u
#define MODBUS_REG_RCAL_BOOT_IMAGE_HI 21u         // FC03: 부팅 RCAL Image
#define MODBUS_REG_RCAL_BOOT_IMAGE_LO 22u
#define MODBUS_REG_RCAL_BOOT_MAG_HI 23u           // FC03: 부팅 RCAL Mag mΩ
#define MODBUS_REG_RCAL_BOOT_MAG_LO 24u
#define MODBUS_REG_RCAL_EEPROM_REAL_HI 25u        // FC03/FC06: EEPROM RCAL Real
#define MODBUS_REG_RCAL_EEPROM_REAL_LO 26u
#define MODBUS_REG_RCAL_EEPROM_IMAGE_HI 27u       // FC03/FC06: EEPROM RCAL Image
#define MODBUS_REG_RCAL_EEPROM_IMAGE_LO 28u
#define MODBUS_REG_RCAL_EEPROM_MAG_HI 29u         // FC03: EEPROM RCAL Mag mΩ (계산값)
#define MODBUS_REG_RCAL_EEPROM_MAG_LO 30u
#define MODBUS_REG_IMP_BASE_START 80u             // FC03/FC06: 셀 기준저항(base) 시작 주소
#define MODBUS_REG_IMP_COMP_START 100u            // FC03/FC06: 셀 보정값(compensation) 시작 (셀1=100 … 셀20=119)
#define MODBUS_REG_IMP_COMP_CELLS MAX_INSTALLED_CELLS
#define MODBUS_REG_IMP_GAIN 120u                  // FC03/FC06: 전역 내부저항 gain(per-mille)
#define MODBUS_REG_IMP_OFFSET 121u                // FC03/FC06: 전역 내부저항 offset(centi-mOhm, int16)
#define MODBUS_REG_FC03_MAX MODBUS_REG_IMP_OFFSET // FC03 최대 주소
#define MODBUS_IMP_READ_MAX_DEFAULT 60u           // 내부저항 최대 읽기 횟수 기본값
#define MODBUS_IMP_READ_MAX_MAX 120u              // 내부저항 최대 읽기 횟수 상한
#define MODBUS_IMP_STABLE_WINDOW_DEFAULT 5u       // 내부저항 안정 판단 윈도우 기본값
#define MODBUS_IMP_STABLE_WINDOW_MAX 20u          // 내부저항 안정 판단 윈도우 상한
#define MODBUS_IMP_GAIN_DEFAULT_PERMILLE 1000u
#define MODBUS_IMP_GAIN_MIN_PERMILLE 100u
#define MODBUS_IMP_GAIN_MAX_PERMILLE 4000u
/** FC03/FC06 보정·설정 (저장 대상은 systemDefaultValue, 나머지는 런타임 값). */
static struct
{
  uint16_t refVoltMv;
  int16_t tempOffset;
  int16_t totalVoltageOffset;
  uint16_t totalVoltageGain;
} s_modbusRuntime = {
    2048u,  // refVoltMv
    0,      // tempOffset
    0,      // totalVoltageOffset
    1000u,  // totalVoltageGain (1.000배)
};

static uint16_t modbusUseHoleCt(void)
{
  return systemDefaultValue.useHoleCt;
}

static int16_t modbusAmpereOffset(void)
{
  return systemDefaultValue.ampereOffset;
}

static uint16_t modbusAmpereGain(void)
{
  return systemDefaultValue.ampereGain == 0u ? 1000u : systemDefaultValue.ampereGain;
}

static uint16_t modbusCellGain(void)
{
  return systemDefaultValue.cellGain == 0u ? 7506u : systemDefaultValue.cellGain;
}

static int16_t modbusCellOffset(void)
{
  return systemDefaultValue.cellOffset;
}

static void modbusSyncCtRatedAmpsFromReg(void)
{
  Ads1220CtScale scale;
  Ads1220_getCtScale(&scale);
  /* 0=센서 없음 → ctRatedAmps 0으로 두면 voltsToAmperes()가 0 반환 */
  scale.ctRatedAmps = (float)modbusUseHoleCt();
  scale.gain = 1.0f;
  Ads1220_setCtScale(&scale);
}

static void modbusSyncVoltageCalibFromReg(void)
{
  const float gainRatio = (float)modbusCellGain() / 1000.0f;
  const float offsetVolts = (float)modbusCellOffset() / 1000.0f;
  Ads1220_setVoltageCalibration(gainRatio, offsetVolts);
}

static bool modbusCurrentSensorEnabled(void)
{
  return modbusUseHoleCt() != 0u;
}

static void modbusParseFirmwareVersion(uint16_t *major, uint16_t *minor, uint16_t *patch)
{
  int ma = 0;
  int mi = 0;
  int pa = 0;
  sscanf(VERSION, "%d.%d.%d", &ma, &mi, &pa);
  if (major)
    *major = (uint16_t)ma;
  if (minor)
    *minor = (uint16_t)mi;
  if (patch)
    *patch = (uint16_t)pa;
}

static uint16_t modbusInstalledCells(void)
{
  uint16_t n = systemDefaultValue.installed_cells;
  if (n < 1)
    n = 1;
  if (n > MODBUS_MAX_CELLS)
    n = MODBUS_MAX_CELLS;
  return n;
}

static uint16_t impMohmToModbusReg(float z_mOhm)
{
  if (z_mOhm <= 0.0f)
    return 0;
  const float v = z_mOhm * 100.0f + 0.5f;
  if (v > 65535.0f)
    return 65535u;
  return (uint16_t)v;
}

static uint16_t impCentiToModbusReg(int16_t centi)
{
  if (centi <= 0)
    return 0;
  return (uint16_t)min((int)centi, 65535);
}

/** RCAL Real/Image/Mag: int32 milli 단위 (표시값 = 레지스터 / 1000.0). HI=상위16비트, LO=하위16비트. */
static int32_t modbusFloatToMilli(float value)
{
  if (!isfinite(value))
    return 0;
  const float rounded = value * 1000.0f + (value >= 0.0f ? 0.5f : -0.5f);
  if (rounded > 2147483647.0f)
    return INT32_MAX;
  if (rounded < -2147483648.0f)
    return INT32_MIN;
  return (int32_t)rounded;
}

static float modbusMilliToFloat(int32_t milli)
{
  return (float)milli / 1000.0f;
}

static void modbusPackI32Milli(uint16_t *reg, uint16_t hiAddr, int32_t milli)
{
  reg[hiAddr] = (uint16_t)((uint32_t)milli >> 16);
  reg[hiAddr + 1u] = (uint16_t)((uint32_t)milli & 0xFFFFu);
}

static void modbusPackFloatMilli(uint16_t *reg, uint16_t hiAddr, float value)
{
  modbusPackI32Milli(reg, hiAddr, modbusFloatToMilli(value));
}

static float modbusRcalMagnitudeMohm(float real, float image)
{
  return sqrtf(real * real + image * image);
}

static bool modbusWriteRcalFloatPair(uint16_t addr, uint16_t word, float *target)
{
  int32_t milli = modbusFloatToMilli(*target);
  if (addr == MODBUS_REG_RCAL_EEPROM_REAL_HI || addr == MODBUS_REG_RCAL_EEPROM_IMAGE_HI)
    milli = ((int32_t)word << 16) | (milli & 0xFFFF);
  else
    milli = (milli & (int32_t)0xFFFF0000) | (uint16_t)word;
  *target = modbusMilliToFloat(milli);
  return true;
}

static uint16_t modbusSanitizeImpGainPermille(uint16_t gainPermille)
{
  if (gainPermille < MODBUS_IMP_GAIN_MIN_PERMILLE || gainPermille > MODBUS_IMP_GAIN_MAX_PERMILLE)
    return (uint16_t)MODBUS_IMP_GAIN_DEFAULT_PERMILLE;
  return gainPermille;
}

static float modbusImpCentiToMohmSigned(int16_t centi)
{
  return (float)centi / 100.0f;
}

static int16_t modbusImpMohmToCentiClamp(float mohms)
{
  float scaled = mohms * 100.0f;
  if (scaled > 32767.0f)
    scaled = 32767.0f;
  if (scaled < -32768.0f)
    scaled = -32768.0f;
  return (int16_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
}

static float modbusRemoveGlobalCalFromMohm(float adjustedMohm, float gain, float offsetMohm)
{
  if (gain < 0.0001f)
    gain = 1.0f;
  const float raw = (adjustedMohm - offsetMohm) / gain;
  return raw > 0.0f ? raw : 0.0f;
}

static float modbusApplyGlobalCalToMohm(float rawMohm, float gain, float offsetMohm)
{
  const float adjusted = rawMohm * gain + offsetMohm;
  return adjusted > 0.0f ? adjusted : 0.0f;
}

static void modbusRecalcImpedanceByGlobalCalChange(uint16_t oldGainPermille, int16_t oldOffsetCenti,
                                                    uint16_t newGainPermille, int16_t newOffsetCenti)
{
  const float oldGain = (float)modbusSanitizeImpGainPermille(oldGainPermille) / 1000.0f;
  const float oldOffsetMohm = modbusImpCentiToMohmSigned(oldOffsetCenti);
  const float newGain = (float)modbusSanitizeImpGainPermille(newGainPermille) / 1000.0f;
  const float newOffsetMohm = modbusImpCentiToMohmSigned(newOffsetCenti);

  for (int i = 0; i < MAX_INSTALLED_CELLS; i++)
  {
    const float oldBaseMohm = modbusImpCentiToMohmSigned(systemDefaultValue.baseImpendance[i]);
    if (oldBaseMohm > 0.0f)
    {
      const float rawBaseMohm = modbusRemoveGlobalCalFromMohm(oldBaseMohm, oldGain, oldOffsetMohm);
      const float newBaseMohm = modbusApplyGlobalCalToMohm(rawBaseMohm, newGain, newOffsetMohm);
      int16_t newBaseCenti = modbusImpMohmToCentiClamp(newBaseMohm);
      if (newBaseCenti < 0)
        newBaseCenti = 0;
      systemDefaultValue.baseImpendance[i] = newBaseCenti;
      cellvalue[i].baseImpendance = newBaseCenti;
    }

    const float compMohm = modbusImpCentiToMohmSigned(systemDefaultValue.impendanceCompensation[i]);
    const float currentMohm = cellvalue[i].impendance;
    if (currentMohm > 0.0f)
    {
      const float oldBeforeCellComp = currentMohm - compMohm;
      const float rawCurrentMohm = modbusRemoveGlobalCalFromMohm(oldBeforeCellComp, oldGain, oldOffsetMohm);
      const float newBeforeCellComp = modbusApplyGlobalCalToMohm(rawCurrentMohm, newGain, newOffsetMohm);
      const float newCurrentMohm = newBeforeCellComp + compMohm;
      cellvalue[i].impendance = newCurrentMohm > 0.0f ? newCurrentMohm : 0.0f;
    }
  }
}

static uint16_t modbusSanitizeImpReadMax(uint16_t v)
{
  if (v < 1u || v > MODBUS_IMP_READ_MAX_MAX)
    return (uint16_t)MODBUS_IMP_READ_MAX_DEFAULT;
  return v;
}

static uint16_t modbusSanitizeImpStableWindow(uint16_t v, uint16_t readMax)
{
  if (v < 2u || v > MODBUS_IMP_STABLE_WINDOW_MAX)
    v = (uint16_t)MODBUS_IMP_STABLE_WINDOW_DEFAULT;
  if (v > readMax)
    v = readMax;
  if (v < 2u)
    v = 2u;
  return v;
}

static uint16_t modbusOpenWireStatus(const _cell_value *cells)
{
  uint16_t mask = 0;
  const uint16_t n = modbusInstalledCells();
  for (uint16_t i = 0; i < n; i++)
  {
    if (cells[i].voltage < 0.6f)
      mask |= (uint16_t)(1u << i);
  }
  return mask;
}

static int32_t modbusScaleS16(int32_t raw, int16_t offset, uint16_t gain)
{
  if (gain == 0)
    gain = 1000;
  return (raw + (int32_t)offset) * (int32_t)gain / 1000;
}

static uint16_t modbusPackTotalVoltageMv(const _cell_value *cells)
{
  uint32_t sum = 0;
  const uint16_t n = modbusInstalledCells();
  for (uint16_t i = 0; i < n; i++)
  {
    const float v = cells[i].voltage;
    if (v > 0.0f)
      sum += (uint32_t)(v * 1000.0f + 0.5f);
  }
  int32_t scaled = modbusScaleS16((int32_t)sum, s_modbusRuntime.totalVoltageOffset, s_modbusRuntime.totalVoltageGain);
  if (scaled < 0)
    scaled = 0;
  if (scaled > 65535)
    return 65535u;
  return (uint16_t)scaled;
}

static void modbusFillFc03Holding(uint16_t *reg, unsigned count)
{
  if (count <= MODBUS_REG_FC03_MAX)
    return;
  dataSyncLockSystemConfig();

  uint16_t maj = 0;
  uint16_t min = 0;
  uint16_t pat = 0;
  modbusParseFirmwareVersion(&maj, &min, &pat);

  reg[0] = (uint16_t)systemDefaultValue.modbusId;
  reg[1] = modbusInstalledCells();
  reg[2] = s_modbusRuntime.refVoltMv;
  reg[3] = modbusCellGain();
  reg[4] = (uint16_t)(int16_t)modbusCellOffset();
  reg[5] = maj;
  reg[6] = min;
  reg[7] = pat;
  _cell_value snap[MAX_INSTALLED_CELLS] = {0};
  dataSyncReadCellSnapshot(snap, MAX_INSTALLED_CELLS);
  reg[8] = modbusOpenWireStatus(snap);
  reg[9] = modbusUseHoleCt();
  reg[10] = (uint16_t)(int16_t)s_modbusRuntime.tempOffset;
  reg[11] = (uint16_t)(int16_t)modbusAmpereOffset();
  reg[12] = modbusAmpereGain();
  reg[13] = (uint16_t)(int16_t)s_modbusRuntime.totalVoltageOffset;
  reg[14] = s_modbusRuntime.totalVoltageGain;
  reg[MODBUS_REG_IMP_READ_MAX] = modbusSanitizeImpReadMax(systemDefaultValue.ACVoltPP);
  reg[MODBUS_REG_IMP_STABLE_WINDOW] = modbusSanitizeImpStableWindow(systemDefaultValue.DCVolt, reg[MODBUS_REG_IMP_READ_MAX]);
  reg[MODBUS_REG_IMP_EEPROM_CHANGE_PERCENT] = (uint16_t)constrain((int)systemDefaultValue.ImpedanceFactor, 1, 100);
  reg[MODBUS_REG_IMP_PERIOD_MIN] = systemDefaultValue.ImpedanceMeasurePeriod == 0 ? 60u : systemDefaultValue.ImpedanceMeasurePeriod;
  modbusPackFloatMilli(reg, MODBUS_REG_RCAL_BOOT_REAL_HI, bootRcalVerifyRealGet());
  modbusPackFloatMilli(reg, MODBUS_REG_RCAL_BOOT_IMAGE_HI, bootRcalVerifyImageGet());
  modbusPackFloatMilli(reg, MODBUS_REG_RCAL_BOOT_MAG_HI, bootRcalVerifyMagnitudeGet());
  modbusPackFloatMilli(reg, MODBUS_REG_RCAL_EEPROM_REAL_HI, systemDefaultValue.real_Cal);
  modbusPackFloatMilli(reg, MODBUS_REG_RCAL_EEPROM_IMAGE_HI, systemDefaultValue.image_Cal);
  modbusPackFloatMilli(reg, MODBUS_REG_RCAL_EEPROM_MAG_HI,
                       modbusRcalMagnitudeMohm(systemDefaultValue.real_Cal, systemDefaultValue.image_Cal));
  reg[MODBUS_REG_BASE_IMP_PROGRESS] = modbusReg50BaseImpProgress;
  for (uint16_t i = 0; i < MODBUS_MAX_CELLS; i++)
    reg[MODBUS_REG_IMP_BASE_START + i] = impCentiToModbusReg(systemDefaultValue.baseImpendance[i]);
  for (uint16_t i = 0; i < MODBUS_REG_IMP_COMP_CELLS; i++)
    reg[MODBUS_REG_IMP_COMP_START + i] = (uint16_t)systemDefaultValue.impendanceCompensation[i];
  reg[MODBUS_REG_IMP_GAIN] = modbusSanitizeImpGainPermille(systemDefaultValue.impedanceGainPermille);
  reg[MODBUS_REG_IMP_OFFSET] = (uint16_t)systemDefaultValue.impedanceOffsetCentiMohm;
  dataSyncUnlockSystemConfig();
}

static void modbusFillFc04Input(uint16_t *reg, unsigned count)
{
  if (count < 96)
    return;

  _cell_value snap[MAX_INSTALLED_CELLS] = {0};
  dataSyncReadCellSnapshot(snap, MAX_INSTALLED_CELLS);
  const uint16_t n = modbusInstalledCells();

  for (uint16_t i = 0; i < MODBUS_MAX_CELLS; i++)
  {
    if (i < n)
    {
      int32_t mv = (int32_t)(snap[i].voltage * 1000.0f + 0.5f);
      if (mv < 0)
        mv = 0;
      if (mv > 65535)
        mv = 65535;
      reg[i] = (uint16_t)mv;
    }
    else
    {
      reg[i] = 0;
    }
  }

  for (int t = 0; t < 2; t++)
  {
    int32_t tx10 = modbusScaleS16(ntcTemperatureC_x10[t], s_modbusRuntime.tempOffset, 1000);
    if (tx10 < -32768)
      tx10 = -32768;
    if (tx10 > 32767)
      tx10 = 32767;
    reg[16 + t] = (uint16_t)(int16_t)tx10;
  }

  {
    int32_t ax10 = 0;
    if (modbusCurrentSensorEnabled())
      ax10 = modbusScaleS16(packCurrentA_x10, modbusAmpereOffset(), modbusAmpereGain());
    if (ax10 < -32768)
      ax10 = -32768;
    if (ax10 > 32767)
      ax10 = 32767;
    reg[18] = (uint16_t)(int16_t)ax10;
  }

  reg[19] = modbusPackTotalVoltageMv(snap);

  for (uint16_t i = 0; i < MODBUS_MAX_CELLS; i++)
  {
    if (i < n)
      reg[60 + i] = impMohmToModbusReg(snap[i].impendance);
    else
      reg[60 + i] = 0;
  }

  dataSyncLockSystemConfig();
  for (uint16_t i = 0; i < MODBUS_MAX_CELLS; i++)
  {
    if (i < n)
      reg[80 + i] = impCentiToModbusReg(systemDefaultValue.baseImpendance[i]);
    else
      reg[80 + i] = 0;
  }
  dataSyncUnlockSystemConfig();
}

static bool modbusAddressRangeOk(uint16_t address, uint16_t words, uint16_t maxAddr)
{
  if (words == 0)
    return false;
  return ((uint32_t)address + (uint32_t)words) <= ((uint32_t)maxAddr + 1u);
}

static ModbusMessage modbusReadRegisters(ModbusMessage request, uint8_t fc, uint16_t maxAddr,
                                       void (*fill)(uint16_t *, unsigned))
{
  uint16_t address = 0;
  uint16_t words = 0;
  request.get(2, address);
  request.get(4, words);

  ModbusMessage response;
  if (!modbusAddressRangeOk(address, words, maxAddr))
  {
    response.setError(request.getServerID(), fc, ILLEGAL_DATA_ADDRESS);
    return response;
  }

  uint16_t buf[256];
  memset(buf, 0, sizeof(buf));
  fill(buf, 256);

  response.add(request.getServerID(), fc, (uint8_t)(words * 2));
  for (uint16_t i = 0; i < words; i++)
    response.add(buf[address + i]);
  return response;
}

static bool modbusWriteHolding(uint16_t addr, uint16_t value, bool *needReboot)
{
  if (needReboot)
    *needReboot = false;

  switch (addr)
  {
  case 0:
    (void)value;
    systemDefaultValue.modbusId = get485Address();
    return true;
  case 1:
    if (value < 1 || value > MODBUS_MAX_CELLS)
      return false;
    systemDefaultValue.installed_cells = value;
    return true;
  case 2:
    s_modbusRuntime.refVoltMv = value;
    return true;
  case 3:
    modbusSetCellGain(value);
    return true;
  case 4:
    modbusSetCellOffset((int16_t)value);
    return true;
  case 9:
    modbusSetUseHoleCt(value);
    return true;
  case 10:
    s_modbusRuntime.tempOffset = (int16_t)value;
    return true;
  case 11:
    modbusSetAmpereOffset((int16_t)value);
    return true;
  case 12:
    modbusSetAmpereGain(value);
    return true;
  case 13:
    s_modbusRuntime.totalVoltageOffset = (int16_t)value;
    return true;
  case 14:
    if (value == 0)
      value = 1000;
    s_modbusRuntime.totalVoltageGain = value;
    return true;
  case MODBUS_REG_IMP_READ_MAX:
    if (value < 1 || value > MODBUS_IMP_READ_MAX_MAX)
      return false;
    systemDefaultValue.ACVoltPP = value;
    systemDefaultValue.DCVolt = modbusSanitizeImpStableWindow(systemDefaultValue.DCVolt, value);
    return true;
  case MODBUS_REG_IMP_STABLE_WINDOW:
    if (value < 2 || value > MODBUS_IMP_STABLE_WINDOW_MAX)
      return false;
    systemDefaultValue.DCVolt = modbusSanitizeImpStableWindow(value, modbusSanitizeImpReadMax(systemDefaultValue.ACVoltPP));
    return true;
  case MODBUS_REG_IMP_EEPROM_CHANGE_PERCENT:
    if (value < 1 || value > 100)
      return false;
    systemDefaultValue.ImpedanceFactor = (uint8_t)value;
    return true;
  case MODBUS_REG_IMP_PERIOD_MIN:
    if (value < 1 || value > 65535)
      return false;
    systemDefaultValue.ImpedanceMeasurePeriod = value;
    return true;
  case MODBUS_REG_RCAL_EEPROM_REAL_HI:
  case MODBUS_REG_RCAL_EEPROM_REAL_LO:
    return modbusWriteRcalFloatPair(addr, value, &systemDefaultValue.real_Cal);
  case MODBUS_REG_RCAL_EEPROM_IMAGE_HI:
  case MODBUS_REG_RCAL_EEPROM_IMAGE_LO:
    return modbusWriteRcalFloatPair(addr, value, &systemDefaultValue.image_Cal);
  case MODBUS_REG_BASE_IMP_PROGRESS:
    modbusOnFc06Reg50Write(value);
    return (value == 0 || value == 1);
  case MODBUS_REG_IMP_GAIN:
    if (value < MODBUS_IMP_GAIN_MIN_PERMILLE || value > MODBUS_IMP_GAIN_MAX_PERMILLE)
      return false;
    modbusRecalcImpedanceByGlobalCalChange(systemDefaultValue.impedanceGainPermille,
                                           systemDefaultValue.impedanceOffsetCentiMohm,
                                           value,
                                           systemDefaultValue.impedanceOffsetCentiMohm);
    systemDefaultValue.impedanceGainPermille = value;
    return true;
  case MODBUS_REG_IMP_OFFSET:
    modbusRecalcImpedanceByGlobalCalChange(systemDefaultValue.impedanceGainPermille,
                                           systemDefaultValue.impedanceOffsetCentiMohm,
                                           systemDefaultValue.impedanceGainPermille,
                                           (int16_t)value);
    systemDefaultValue.impedanceOffsetCentiMohm = (int16_t)value;
    return true;
  default:
    if (addr >= MODBUS_REG_IMP_BASE_START && addr < (MODBUS_REG_IMP_BASE_START + MODBUS_MAX_CELLS))
    {
      if (value > 32767u)
        return false;
      const uint16_t idx = (uint16_t)(addr - MODBUS_REG_IMP_BASE_START);
      systemDefaultValue.baseImpendance[idx] = (int16_t)value;
      cellvalue[idx].baseImpendance = (int16_t)value;
      return true;
    }
    if (addr >= MODBUS_REG_IMP_COMP_START &&
        addr < (MODBUS_REG_IMP_COMP_START + MODBUS_REG_IMP_COMP_CELLS))
    {
      const uint16_t idx = (uint16_t)(addr - MODBUS_REG_IMP_COMP_START);
      const int16_t oldComp = systemDefaultValue.impendanceCompensation[idx];
      const int16_t newComp = (int16_t)value;
      systemDefaultValue.impendanceCompensation[idx] = newComp;
      cellvalue[idx].impendanceCompensation = newComp;
      const float deltaMohm = (float)(newComp - oldComp) / 100.0f;
      if (cellvalue[idx].impendance > 0.0f)
      {
        const float zAdjusted = cellvalue[idx].impendance + deltaMohm;
        cellvalue[idx].impendance = zAdjusted > 0.0f ? zAdjusted : 0.0f;
      }
      return true;
    }
    return false;
  }
}

ModbusMessage syncRequestCellModule(uint32_t token, uint8_t modbusId, uint8_t fCode,
                                    uint16_t startAddress, uint16_t len)
{
  (void)token;
  ESP_LOGW("MODBUS", "syncRequestCellModule stub: id=%u fc=%u addr=%u data=%u",
           modbusId, fCode, startAddress, len);

  ModbusMessage rsp;
  switch (fCode)
  {
  case READ_COIL:
    rsp.add(modbusId, READ_COIL);
    rsp.add((uint8_t)1);
    rsp.add((uint8_t)0);
    break;
  case WRITE_COIL:
    rsp.add(modbusId, WRITE_COIL);
    rsp.add(startAddress);
    rsp.add(len);
    break;
  case WRITE_HOLD_REGISTER:
    rsp.add(modbusId, WRITE_HOLD_REGISTER);
    rsp.add(startAddress);
    rsp.add(len);
    break;
  default:
    rsp.setError(modbusId, fCode, ILLEGAL_FUNCTION);
    break;
  }
  return rsp;
}

void setErrorMessageToModbus(bool setError, const char *msg)
{
  memset(strErrorMessage, 0x00, sizeof(strErrorMessage));
  if (setError)
  {
    strErrorMessage[0] = 0;
    strErrorMessage[1] = setError;
    strncpy(strErrorMessage + 2, msg, sizeof(strErrorMessage) - 3);
    strErrorMessage[sizeof(strErrorMessage) - 1] = '\0';
  }
  else
  {
    strErrorMessage[0] = 0;
    strErrorMessage[1] = 0;
  }
}

bool modbusBaselineScanIsActive(void)
{
  return modbusReg50BaseImpProgress != 0;
}

bool modbusHasCurrentSensor(void)
{
  return modbusCurrentSensorEnabled();
}

uint16_t modbusGetUseHoleCt(void)
{
  return modbusUseHoleCt();
}

int16_t modbusGetAmpereOffset(void)
{
  return modbusAmpereOffset();
}

uint16_t modbusGetAmpereGain(void)
{
  return modbusAmpereGain();
}

uint16_t modbusGetCellGain(void)
{
  return modbusCellGain();
}

int16_t modbusGetCellOffset(void)
{
  return modbusCellOffset();
}

void modbusSetUseHoleCt(uint16_t value)
{
  systemDefaultValue.useHoleCt = value;
  modbusSyncCtRatedAmpsFromReg();
}

void modbusSetAmpereOffset(int16_t value)
{
  systemDefaultValue.ampereOffset = value;
}

void modbusSetAmpereGain(uint16_t value)
{
  if (value == 0u)
    value = 1000u;
  systemDefaultValue.ampereGain = value;
}

void modbusSetCellGain(uint16_t value)
{
  if (value == 0u)
    value = 7506u;
  systemDefaultValue.cellGain = value;
  modbusSyncVoltageCalibFromReg();
}

void modbusSetCellOffset(int16_t value)
{
  systemDefaultValue.cellOffset = value;
  modbusSyncVoltageCalibFromReg();
}

ModbusMessage FC03(ModbusMessage request)
{
  return modbusReadRegisters(request, READ_HOLD_REGISTER, MODBUS_REG_FC03_MAX,
                             modbusFillFc03Holding);
}

ModbusMessage FC04(ModbusMessage request)
{
  return modbusReadRegisters(request, READ_INPUT_REGISTER, 95u, modbusFillFc04Input);
}

ModbusMessage FC01(ModbusMessage request)
{
  ModbusMessage response;
  response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
  return response;
}

ModbusMessage FC05(ModbusMessage request)
{
  ModbusMessage response;
  response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
  return response;
}

ModbusMessage FC06(ModbusMessage request)
{
  uint16_t address;
  ModbusMessage response;
  uint16_t value;
  request.get(2, address);
  request.get(4, value);
  const uint16_t writeAddress = (0xFFFF & address);

  response.add(request.getServerID(), request.getFunctionCode(), writeAddress);
  response.add(value);

  ESP_LOGD("MODBUS", "FC06 addr=%u val=%u", writeAddress, value);

  const bool isCoreHoldingRange =
      (writeAddress <= MODBUS_REG_IMP_PERIOD_MIN) ||
      (writeAddress >= MODBUS_REG_RCAL_BOOT_REAL_HI && writeAddress <= MODBUS_REG_RCAL_EEPROM_MAG_LO) ||
      (writeAddress == MODBUS_REG_BASE_IMP_PROGRESS) ||
      (writeAddress >= MODBUS_REG_IMP_BASE_START && writeAddress <= MODBUS_REG_IMP_OFFSET);

  if (isCoreHoldingRange)
  {
    if (writeAddress >= 5 && writeAddress <= 8)
    {
      response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
      return response;
    }
    bool needReboot = false;
    dataSyncLockSystemConfig();
    if (!modbusWriteHolding(writeAddress, value, &needReboot))
    {
      dataSyncUnlockSystemConfig();
      response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
      return response;
    }
    if (writeAddress == 0 || writeAddress == 1 ||
        (writeAddress >= 2 && writeAddress <= 4) ||
        (writeAddress >= 9 && writeAddress <= MODBUS_REG_IMP_PERIOD_MIN) ||
        (writeAddress >= MODBUS_REG_RCAL_EEPROM_REAL_HI && writeAddress <= MODBUS_REG_RCAL_EEPROM_IMAGE_LO) ||
        (writeAddress >= MODBUS_REG_IMP_BASE_START && writeAddress <= MODBUS_REG_IMP_OFFSET))
    {
      (void)readnWriteEEProm(true);
    }
    if (writeAddress >= MODBUS_REG_RCAL_EEPROM_REAL_HI && writeAddress <= MODBUS_REG_RCAL_EEPROM_IMAGE_LO)
      AD5940_UseStoredRcalFromEeprom();
    dataSyncUnlockSystemConfig();
    if (needReboot)
      ESP.restart();
    return response;
  }

  response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
  return response;
}
