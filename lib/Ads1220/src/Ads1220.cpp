#include "Ads1220.h"
#include <SPI.h>
#include <esp_log.h>

#define VOLTAGE_OFFSET 0.356
#define AMPERAGE_OFFSET 0.000f
#define VOLTAGE_GAIN_RATIO 7.506f
/** AIN2 핀 전압 = ADC 환산값 (멀티미터와 동일, ×2 하지 않음) */
#define AMPERAGE_GAIN_RATIO 2.0f
#define ADS1220_VREF_V 2.048f
/** CT 정격(기본 200A)일 때 AIN2 전압(V) — 테스터·부하로 ctCurrentInit/setCtScale 보정 */
#define ADS1220_CT_RATED_AMPS_DEFAULT 200.0f
static const char *TAG = "Ads1220";

static Ads1220CtScale s_ctScale = {
    ADS1220_CT_RATED_AMPS_DEFAULT,
    ADS1220_CT_CENTER_VOLTS_DEFAULT,
    ADS1220_CT_VOLTS_SPAN_DEFAULT,
    1.0f,
};

/* ADS1220 SPI commands (TI SBAS501) */
static constexpr uint8_t kCmdReset = 0x06u;
static constexpr uint8_t kCmdStartSync = 0x08u;
static constexpr uint8_t kCmdRdata = 0x10u;

float Ads1220VolateCompensation(float voltage)
{
  if(voltage < 1.3)
    return -0.0431f;
  else if(voltage < 4.0)
    return -0.0431f + (voltage - 1.3) * (0.0431-0.0584) / (4.0-1.3);
  else if(voltage < 6.0)
    return -0.0478f + (voltage - 4.0) * (0.0478-0.0431) / (6.0-4.0);
  else if(voltage < 8.0)
    return -0.0500f + (voltage - 6.0) * (0.0500-0.0478) / (8.0-6.0);
  else if(voltage < 10.0)
    return -0.0600f + (voltage - 8.0) * (0.0730-0.0700) / (10.0-8.0);
  else if(voltage < 12.0)
    return -0.0610f + (voltage - 10.0) * (0.0620-0.0500) / (12.0-10.0);
  else if(voltage < 14.0)
    return -0.0610f + (voltage - 12.0) * (0.0610-0.0620) / (14.0-12.0);
  else if(voltage < 16.0)
    return -0.0610f + (voltage - 14.0) * (0.0610-0.0610) / (16.0-14.0);
  else
    return -0.0610f + (voltage - 16.0) * (0.0610-0.0610) / (18.0-16.0);
}

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
  (void)Ads1220_applyConfigSingleEnded(0);
}

void Ads1220_applyConfigAin1Avss(void)
{
  (void)Ads1220_applyConfigSingleEnded(1);
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

int32_t Ads1220_readAveragedRaw(uint8_t samples, uint32_t timeoutMsPerSample, uint32_t interSampleDelayUs)
{
  if (!s_ready)
    return 0;
  if (samples == 0)
    samples = 1;

  int64_t sum = 0;
  uint8_t okCount = 0;
  for (uint8_t i = 0; i < samples; i++)
  {
    if (interSampleDelayUs > 0)
      delayMicroseconds(interSampleDelayUs);
    Ads1220_startSync();
    if (!Ads1220_waitDrdy(timeoutMsPerSample))
      continue;
    sum += (int64_t)Ads1220_readRaw();
    okCount++;
  }

  if (okCount == 0)
  {
    ESP_LOGW(TAG, "readAveragedRaw failed: no valid samples (%u)", (unsigned)samples);
    return 0;
  }

  return (int32_t)(sum / (int64_t)okCount);
}
/* 채널 전환 직후 첫 변환값은 버리고 안정화 
   Voltage : channel 0
   Amperage : channel 2
   channel 1,3 : Reserved
   parameter : ainIndex(0:Voltage, 2:Amperage)
   samples : 변환 횟수
   timeoutMsPerSample : 각 변환의 타임아웃(ms)
   interSampleDelayUs : 각 변환 사이의 딜레이(us)
*/
int32_t Ads1220_readAveragedRawOnChannel(uint8_t ainIndex, uint8_t samples, uint32_t timeoutMsPerSample, uint32_t interSampleDelayUs)
{
  if (!Ads1220_applyConfigSingleEnded(ainIndex))
    return 0;

  /* 채널 전환 직후 첫 변환값은 버리고 안정화 */
  Ads1220_startSync();
  (void)Ads1220_waitDrdy(timeoutMsPerSample);
  (void)Ads1220_readRaw();
  int32_t result = Ads1220_readAveragedRaw(samples, timeoutMsPerSample, interSampleDelayUs);
  Ads1220_end();
  return result;
}

float Ads1220_readAveragedVoltageOnChannel(uint8_t ainIndex, uint8_t samples, uint32_t timeoutMsPerSample, uint32_t interSampleDelayUs)
{
  const int32_t raw = Ads1220_readAveragedRawOnChannel(ainIndex, samples, timeoutMsPerSample, interSampleDelayUs);
  const float voltage = Ads1220_rawToVoltsWithOffset(raw, 1, VOLTAGE_GAIN_RATIO);
  const float compensation = Ads1220VolateCompensation(voltage);
  return voltage + compensation;
}

void Ads1220_setCtScale(const Ads1220CtScale *scale)
{
  if (scale == nullptr)
    return;
  s_ctScale = *scale;
  if (s_ctScale.ctRatedAmps <= 0.0f)
    s_ctScale.ctRatedAmps = ADS1220_CT_RATED_AMPS_DEFAULT;
  if (s_ctScale.centerVolts <= 0.01f)
    s_ctScale.centerVolts = ADS1220_CT_CENTER_VOLTS_DEFAULT;
  if (s_ctScale.voltsSpan <= 0.01f)
    s_ctScale.voltsSpan = ADS1220_CT_VOLTS_SPAN_DEFAULT;
  if (s_ctScale.gain <= 0.0f)
    s_ctScale.gain = 1.0f;
}

void Ads1220_getCtScale(Ads1220CtScale *scaleOut)
{
  if (scaleOut != nullptr)
    *scaleOut = s_ctScale;
}

float Ads1220_rawToVoltsAmperage(int32_t raw24, float pgaGain)
{
  if (pgaGain == 0)
    pgaGain = 1;
  const float scale = ADS1220_VREF_V / (8388608.0f * (float)pgaGain);
  return (float)raw24 * scale * AMPERAGE_GAIN_RATIO;
}

float Ads1220_voltsToAmperes(float voltsAin2, const Ads1220CtScale *scale)
{
  const Ads1220CtScale *sc = (scale != nullptr) ? scale : &s_ctScale;
  if (sc->voltsSpan <= 0.01f || sc->ctRatedAmps <= 0.0f)
    return 0.0f;
  return (voltsAin2 - sc->centerVolts) * (sc->ctRatedAmps / sc->voltsSpan) * sc->gain;
}

float Ads1220_readAveragedCurrentAmps(uint8_t samples, uint32_t timeoutMsPerSample, uint32_t interSampleDelayUs)
{
  const int32_t raw = Ads1220_readAveragedRawOnChannel(
      ADS1220_AIN_CURRENT, samples, timeoutMsPerSample, interSampleDelayUs);
  const float v = Ads1220_rawToVoltsAmperage(raw, 1.0);
  return Ads1220_voltsToAmperes(v, &s_ctScale);
}

// 전압: 130K:2K 분압 → 7.506배. 전류: AIN2 2V=0A, (V-2)*ctRated/2.

float Ads1220_rawToVolts(int32_t raw24, float vrefVolts, uint8_t pgaGain,float gainRatio )
{
  if (pgaGain == 0)
    pgaGain = 1;
  /* 단일단·양의 입력 근사: Code는 24비트 2의 보수, 풀스케일은 Vref/gain 근처 */
  const float scale = vrefVolts / (8388608.0f * (float)pgaGain);
  return (float)raw24 * scale * gainRatio ;
}
float Ads1220_rawToVoltsWithOffset(int32_t raw24, uint8_t pgaGain, float gainRatio)
{
  const float vrefVolts = 2.048f; /* ADS1220 내부 기준, 항상 동일 */
  if (pgaGain == 0)
    pgaGain = 1;
  /* 단일단·양의 입력 근사: Code는 24비트 2의 보수, 풀스케일은 Vref/gain 근처 */
  const float scale = vrefVolts / (8388608.0f * (float)pgaGain);
  return (float)raw24 * scale * gainRatio + VOLTAGE_OFFSET;
}