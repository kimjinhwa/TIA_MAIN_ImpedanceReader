#include "ctCurrent.h"
#include <Ads1220.h>

#include <Arduino.h>

int16_t packCurrentA_x10 = 0;
float packCurrentAin2Volts = 0.0f;

/** main.cpp ADS1220 샘플 설정과 동일 */
static const uint8_t kSamples = 4;
static const uint32_t kTimeoutMs = 500;
static const uint32_t kInterUs = 50;

void ctCurrentInit(void)
{
  const float rated = systemDefaultValue.useHoleCt > 0u
                          ? (float)systemDefaultValue.useHoleCt
                          : 0.0f;
  Ads1220CtScale scale = {
      rated,
      ADS1220_CT_CENTER_VOLTS_DEFAULT,
      ADS1220_CT_VOLTS_SPAN_DEFAULT,
      1.0f,
  };
  Ads1220_setCtScale(&scale);
}

void ctCurrentUpdate(void)
{
  if (systemDefaultValue.useHoleCt == 0u)
  {
    packCurrentAin2Volts = 0.0f;
    packCurrentA_x10 = 0;
    return;
  }

  const int32_t raw = Ads1220_readAveragedRawOnChannel(
      ADS1220_AIN_CURRENT, kSamples, kTimeoutMs, kInterUs);
  packCurrentAin2Volts = Ads1220_rawToVoltsAmperage(raw, 1.0);
  const float amps = Ads1220_voltsToAmperes(packCurrentAin2Volts, nullptr);
  int32_t a10 = (int32_t)(amps * 10.0f + (amps >= 0.0f ? 0.5f : -0.5f));
  if (a10 > 32767)
    a10 = 32767;
  if (a10 < -32768)
    a10 = -32768;
  packCurrentA_x10 = (int16_t)a10;
}

float ctCurrentGetAmps(void)
{
  return (float)packCurrentA_x10 / 10.0f;
}

float ctCurrentGetCalibratedAmps(void)
{
  if (systemDefaultValue.useHoleCt == 0u)
    return 0.0f;

  uint16_t gain = systemDefaultValue.ampereGain;
  if (gain == 0u)
    gain = 1000u;

  const int32_t scaled = ((int32_t)packCurrentA_x10 + (int32_t)systemDefaultValue.ampereOffset)
                         * (int32_t)gain / 1000;
  return (float)scaled / 10.0f;
}
