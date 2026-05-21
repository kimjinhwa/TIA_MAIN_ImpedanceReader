#include "ntcTemperature.h"
#include "mainGrobal.h"

#include <Arduino.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <math.h>

static const char TAG[] = "NTC";

int16_t ntcTemperatureC_x10[2] = {0, 0};

static esp_adc_cal_characteristics_t s_adcChars;

static const adc1_channel_t s_adcCh[2] = {
    ADC1_CHANNEL_6, /* GPIO34 IN_TH1 */
    ADC1_CHANNEL_7, /* GPIO35 IN_TH2 */
};

/** 분압 상단 고정저항·NTC 25°C 저항 (Ω) */
static const float NTC_PULLUP_OHM = 10000.0f;
static const float NTC_R25_OHM = 10000.0f;
/** 일반 10K NTC B값 */
static const float NTC_BETA = 3950.0f;
static const float NTC_T25_K = 298.15f;
static const float NTC_VCC_MV = 3300.0f;
static const int NTC_ADC_SAMPLES = 16;

void ntcTemperatureInit(void)
{
  pinMode(IN_TH1, INPUT);
  pinMode(IN_TH2, INPUT);
  adc1_config_width(ADC_WIDTH_BIT_12);
  for (int i = 0; i < 2; i++)
    adc1_config_channel_atten(s_adcCh[i], ADC_ATTEN_DB_11);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &s_adcChars);
}

static uint32_t readAdcRawAverage(adc1_channel_t ch)
{
  uint32_t sum = 0;
  for (int n = 0; n < NTC_ADC_SAMPLES; n++)
    sum += (uint32_t)adc1_get_raw(ch);
  return sum / (uint32_t)NTC_ADC_SAMPLES;
}

/** 분압: 3.3V — 10K — IN_TH — NTC 10K — GND → V = Vcc·Rntc/(Rpu+Rntc) */
static int16_t milliVoltsToTempCx10(uint32_t mv)
{
  if (mv < 50u || mv + 50u >= (uint32_t)NTC_VCC_MV)
    return 0;

  const float rNtc = NTC_PULLUP_OHM * (float)mv / (NTC_VCC_MV - (float)mv);
  if (rNtc < 100.0f || rNtc > 500000.0f)
    return 0;

  const float invT = (1.0f / NTC_T25_K) + logf(rNtc / NTC_R25_OHM) / NTC_BETA;
  const float tC = (1.0f / invT) - 273.15f;
  int32_t c10 = (int32_t)(tC * 10.0f + (tC >= 0.0f ? 0.5f : -0.5f));
  if (c10 < -400)
    c10 = -400;
  if (c10 > 1500)
    c10 = 1500;
  return (int16_t)c10;
}

static int16_t readSensorTempCx10(uint8_t index)
{
  if (index > 1)
    return 0;
  adc1_config_channel_atten(s_adcCh[index], ADC_ATTEN_DB_11);
  const uint32_t raw = readAdcRawAverage(s_adcCh[index]);
  const uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &s_adcChars);
  return milliVoltsToTempCx10(mv);
}

void ntcTemperatureUpdate(void)
{
  for (uint8_t i = 0; i < 2; i++)
    ntcTemperatureC_x10[i] = readSensorTempCx10(i);
}

int16_t ntcTemperatureGetCx10(uint8_t sensorIndex)
{
  if (sensorIndex > 1)
    return 0;
  return ntcTemperatureC_x10[sensorIndex];
}
