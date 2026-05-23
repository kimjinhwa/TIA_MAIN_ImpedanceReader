#include <Arduino.h>
#include <EEPROM.h>
#include <Ads1220.h>
#include "modbusRtu.h"
#include "mainGrobal.h"
#include "dataSync.h"
#include "eepromNvs.hpp"
#include "../Version.h"
#include <ModbusClientRTU.h>

extern uint8_t get485Address(void);

char strErrorMessage[40];

uint16_t modbusReg50BaseImpProgress = 0;

/** Modbus주소.md — FC04 최대 셀 수 */
#define MODBUS_MAX_CELLS 16u                      // FC04에서 처리하는 최대 셀 수
#define MODBUS_REG_BASE_IMP_PROGRESS 50u          // FC03/FC06: 기준 내부저항 스캔 진행 상태 레지스터
#define MODBUS_REG_IMP_READ_MAX 15u               // FC03/FC06: 내부저항 최대 읽기 횟수 레지스터
#define MODBUS_REG_IMP_STABLE_WINDOW 16u          // FC03/FC06: 내부저항 안정 판단 윈도우 레지스터
#define MODBUS_REG_IMP_EEPROM_CHANGE_PERCENT 17u  // FC03/FC06: EEPROM 갱신 임계치(%) 레지스터
#define MODBUS_REG_IMP_PERIOD_SEC 18u             // FC03/FC06: 내부저항 측정 주기(초) 레지스터
#define MODBUS_IMP_READ_MAX_DEFAULT 60u           // 내부저항 최대 읽기 횟수 기본값
#define MODBUS_IMP_READ_MAX_MAX 120u              // 내부저항 최대 읽기 횟수 상한
#define MODBUS_IMP_STABLE_WINDOW_DEFAULT 5u       // 내부저항 안정 판단 윈도우 기본값
#define MODBUS_IMP_STABLE_WINDOW_MAX 20u          // 내부저항 안정 판단 윈도우 상한
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
  scale.ctRatedAmps = (float)modbusUseHoleCt();
  if (scale.ctRatedAmps <= 0.0f)
    scale.ctRatedAmps = ADS1220_CT_RATED_AMPS_DEFAULT;
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
  if (count < 51)
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
  reg[MODBUS_REG_IMP_PERIOD_SEC] = systemDefaultValue.ImpedanceMeasurePeriod == 0 ? 3600u : systemDefaultValue.ImpedanceMeasurePeriod;
  reg[MODBUS_REG_BASE_IMP_PROGRESS] = modbusReg50BaseImpProgress;
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
  if (fc == READ_HOLD_REGISTER)
  {
    dataSyncLockSystemConfig();
    EEPROM.readBytes(1, (byte *)&systemDefaultValue, sizeof(nvsSystemSet));
    dataSyncUnlockSystemConfig();
  }
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
  case MODBUS_REG_IMP_PERIOD_SEC:
    if (value < 1 || value > 65535)
      return false;
    systemDefaultValue.ImpedanceMeasurePeriod = value;
    return true;
  case MODBUS_REG_BASE_IMP_PROGRESS:
    modbusOnFc06Reg50Write(value);
    return (value == 0 || value == 1);
  default:
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
  return modbusReadRegisters(request, READ_HOLD_REGISTER, MODBUS_REG_BASE_IMP_PROGRESS,
                             modbusFillFc03Holding);
}

ModbusMessage FC04(ModbusMessage request)
{
  return modbusReadRegisters(request, READ_INPUT_REGISTER, 95u, modbusFillFc04Input);
}

ModbusMessage FC01(ModbusMessage request)
{
  uint16_t address;
  ModbusMessage response;
  uint16_t quantity;
  request.get(2, address);
  request.get(4, quantity);
  uint16_t writeAddress = (0xFFFF & address);

  response.add(request.getServerID(), request.getFunctionCode());
  ESP_LOGI("MODBUS", "FC01 address(%u) quantity(%u)", writeAddress, quantity);

  if (writeAddress >= 0x1101 && writeAddress <= 0x2501)
  {
    uint8_t moduleAddress = address >> 8;
    moduleAddress -= 16;
    writeAddress &= 0x00FF;
    writeAddress = writeAddress - 1;
    uint32_t token = millis();
    ModbusMessage rc = syncRequestCellModule(token, moduleAddress, request.getFunctionCode(), writeAddress, quantity);

    std::vector<uint8_t> MM_data(rc.data(), rc.data() + rc.size());
    uint8_t relay = static_cast<uint8_t>(MM_data.size() > 3 ? MM_data[3] : 0);
    response.add((uint8_t)1);
    response.add(relay);
  }
  return response;
}

ModbusMessage FC05(ModbusMessage request)
{
  uint16_t address;
  ModbusMessage response;
  uint16_t value;

  request.get(2, address);
  request.get(4, value);
  uint16_t writeAddress = (0xFFFF & address);

  response.add(request.getServerID(), request.getFunctionCode(), writeAddress);
  response.add(value);

  if (writeAddress >= 0x1101 && writeAddress <= 0x2501)
  {
    uint8_t moduleAddress = address >> 8;
    moduleAddress -= 16;
    writeAddress &= 0x00FF;
    writeAddress = writeAddress - 1;
    uint32_t token = millis();
    syncRequestCellModule(token, moduleAddress, request.getFunctionCode(), writeAddress, value);
  }
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

  ESP_LOGI("MODBUS", "FC06 addr=%u val=%u", writeAddress, value);

  if (writeAddress <= MODBUS_REG_IMP_PERIOD_SEC || writeAddress == MODBUS_REG_BASE_IMP_PROGRESS)
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
        (writeAddress >= 9 && writeAddress <= MODBUS_REG_IMP_PERIOD_SEC))
    {
      (void)readnWriteEEProm(true);
    }
    dataSyncUnlockSystemConfig();
    if (needReboot)
      ESP.restart();
    return response;
  }

  if (writeAddress >= 0x1101 && writeAddress <= 0x2501)
  {
    uint8_t moduleAddress = address >> 8;
    moduleAddress -= 16;
    uint16_t cellAddr = (writeAddress & 0x00FF) - 1;
    syncRequestCellModule(millis(), moduleAddress, request.getFunctionCode(), cellAddr, value);
  }
  else
  {
    response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
  }
  return response;
}
