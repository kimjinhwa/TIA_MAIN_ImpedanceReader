#include "Ads1220.h"
#include <SPI.h>
#include <esp_log.h>

static const char *TAG = "Ads1220";

/* ADS1220 SPI commands (TI SBAS501) */
static constexpr uint8_t kCmdReset = 0x06u;
static constexpr uint8_t kCmdStartSync = 0x08u;
static constexpr uint8_t kCmdRdata = 0x10u;

static inline uint8_t cmdRreg(uint8_t reg, uint8_t numBytes)
{
  return (uint8_t)(0x20u | (uint8_t)((reg & 0x0Fu) << 2) | ((numBytes - 1u) & 0x03u));
}

static inline uint8_t cmdWreg(uint8_t reg, uint8_t numBytes)
{
  return (uint8_t)(0x40u | (uint8_t)((reg & 0x0Fu) << 2) | ((numBytes - 1u) & 0x03u));
}

/* CONFIG0: MUX[7:4]=1000b(AIN0-AVSS), GAIN=001(1), PGA_BYPASS=1 */
static constexpr uint8_t kCfg0_Ain0Avss = 0x81u;
/* CONFIG0: MUX[7:4]=1001b(AIN1-AVSS), GAIN=001(1), PGA_BYPASS=1 */
static constexpr uint8_t kCfg0_Ain1Avss = 0x91u;
/* CONFIG1: DR=20 SPS(000), PGA=1(000) — 데이터시트 기본에 가깝게 */
static constexpr uint8_t kCfg1_Default20SpsGain1 = 0x00u;
static constexpr uint8_t kCfg2_Default = 0x00u;
static constexpr uint8_t kCfg3_Default = 0x00u;

static uint8_t s_cs;
static uint8_t s_drdy;
static uint8_t s_cs5940;
static uint8_t s_mcpCs;
static uint32_t s_spiHz = 1000000u;
static bool s_ready = false;

static void selectAds1220(void)
{
  digitalWrite(s_cs5940, HIGH);
  digitalWrite(s_mcpCs, HIGH);
  digitalWrite(s_cs, LOW);
}

static void releaseCsHigh(void)
{
  digitalWrite(s_cs, HIGH);
}

static void spiBeginMode1(void)
{
  SPI.beginTransaction(SPISettings(s_spiHz, MSBFIRST, SPI_MODE1));
}

static void spiEnd(void)
{
  SPI.endTransaction();
}

void Ads1220_begin(uint8_t csPin, uint8_t drdyPin, uint8_t cs5940Pin, uint8_t mcpCsPin, uint32_t spiClockHz)
{
  s_cs = csPin;
  s_drdy = drdyPin;
  s_cs5940 = cs5940Pin;
  s_mcpCs = mcpCsPin;
  s_spiHz = spiClockHz ? spiClockHz : 1000000u;
  s_ready = true;
}

void Ads1220_end(void)
{
  if (!s_ready)
    return;
  digitalWrite(s_cs, HIGH);
  digitalWrite(s_cs5940, HIGH);
  digitalWrite(s_mcpCs, HIGH);
}

void Ads1220_reset(void)
{
  if (!s_ready)
    return;
  selectAds1220();
  spiBeginMode1();
  SPI.transfer(kCmdReset);
  spiEnd();
  releaseCsHigh();
  delay(1);
}

void Ads1220_writeReg8(uint8_t reg, uint8_t val)
{
  if (!s_ready)
    return;
  selectAds1220();
  spiBeginMode1();
  SPI.transfer(cmdWreg(reg, 1));
  SPI.transfer(val);
  spiEnd();
  releaseCsHigh();
}

static void writeRegsBlock(uint8_t startReg, const uint8_t *data, uint8_t numBytes)
{
  if (!s_ready || !data || numBytes == 0 || numBytes > 4)
    return;
  selectAds1220();
  spiBeginMode1();
  SPI.transfer(cmdWreg(startReg, numBytes));
  for (uint8_t i = 0; i < numBytes; i++)
    SPI.transfer(data[i]);
  spiEnd();
  releaseCsHigh();
}

uint8_t Ads1220_readReg8(uint8_t reg)
{
  if (!s_ready)
    return 0;
  selectAds1220();
  spiBeginMode1();
  SPI.transfer(cmdRreg(reg, 1));
  const uint8_t v = SPI.transfer(0xFFu);
  spiEnd();
  releaseCsHigh();
  return v;
}

void Ads1220_applyConfigAin0Avss(void)
{
  const uint8_t cfg[4] = {
      kCfg0_Ain0Avss,
      kCfg1_Default20SpsGain1,
      kCfg2_Default,
      kCfg3_Default,
  };
  writeRegsBlock(0, cfg, 4);
}

void Ads1220_applyConfigAin1Avss(void)
{
  const uint8_t cfg[4] = {
      kCfg0_Ain1Avss,
      kCfg1_Default20SpsGain1,
      kCfg2_Default,
      kCfg3_Default,
  };
  writeRegsBlock(0, cfg, 4);
}

bool Ads1220_applyConfigSingleEnded(uint8_t ainIndex)
{
  uint8_t cfg0 = 0;
  switch (ainIndex)
  {
  case 0:
    cfg0 = kCfg0_Ain0Avss;
    break;
  case 1:
    cfg0 = kCfg0_Ain1Avss;
    break;
  case 2:
    cfg0 = 0xA1u; /* MUX=1010b: AIN2-AVSS, GAIN=1, BYPASS=1 */
    break;
  case 3:
    cfg0 = 0xB1u; /* MUX=1011b: AIN3-AVSS, GAIN=1, BYPASS=1 */
    break;
  default:
    ESP_LOGW(TAG, "invalid single-ended AIN index: %u", (unsigned)ainIndex);
    return false;
  }

  const uint8_t cfg[4] = {
      cfg0,
      kCfg1_Default20SpsGain1,
      kCfg2_Default,
      kCfg3_Default,
  };
  writeRegsBlock(0, cfg, 4);
  return true;
}

void Ads1220_startSync(void)
{
  if (!s_ready)
    return;
  selectAds1220();
  spiBeginMode1();
  SPI.transfer(kCmdStartSync);
  spiEnd();
  releaseCsHigh();
}

bool Ads1220_waitDrdy(uint32_t timeoutMs)
{
  if (!s_ready)
    return false;
  const uint32_t t0 = millis();
  /* DRDY: 변환 준비 시 보통 LOW (데이터시트 Fig. 35 등) */
  while (digitalRead(s_drdy) == HIGH)
  {
    if ((millis() - t0) >= timeoutMs)
    {
      ESP_LOGW(TAG, "waitDrdy timeout %u ms", (unsigned)timeoutMs);
      return false;
    }
    yield();
  }
  return true;
}

int32_t Ads1220_readRaw(void)
{
  if (!s_ready)
    return 0;
  selectAds1220();
  spiBeginMode1();
  SPI.transfer(kCmdRdata);
  const uint8_t b0 = SPI.transfer(0xFFu);
  const uint8_t b1 = SPI.transfer(0xFFu);
  const uint8_t b2 = SPI.transfer(0xFFu);
  spiEnd();
  releaseCsHigh();

  uint32_t u = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | (uint32_t)b2;
  u &= 0xFFFFFFu;
  if (u & 0x800000u)
    u |= 0xFF000000u;
  return (int32_t)u;
}

float Ads1220_rawToVolts(int32_t raw24, float vrefVolts, uint8_t pgaGain)
{
  if (pgaGain == 0)
    pgaGain = 1;
  /* 단일단·양의 입력 근사: Code는 24비트 2의 보수, 풀스케일은 Vref/gain 근처 */
  const float scale = vrefVolts / (8388608.0f * (float)pgaGain);
  return (float)raw24 * scale;
}
