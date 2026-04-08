#include "Mcp23s08.h"
#include <SPI.h>
#include <esp_log.h>

static const char *TAG = "Mcp23s08";

static constexpr uint8_t kOpWrite = 0x40u; /* A2=A1=A0=0, R/W=0 */
static constexpr uint8_t kRegIODIR = 0x00u;
static constexpr uint8_t kRegOLAT = 0x0Au;

static uint8_t s_mcpCs;
static uint8_t s_cs5940;
static uint8_t s_csAds1220;
static uint32_t s_spiHz = 1000000u;
static bool s_ready = false;

void Mcp23s08_begin(uint8_t mcpCsPin, uint8_t cs5940Pin, uint8_t ads1220CsPin, uint32_t spiClockHz)
{
  s_mcpCs = mcpCsPin;
  s_cs5940 = cs5940Pin;
  s_csAds1220 = ads1220CsPin;
  s_spiHz = spiClockHz ? spiClockHz : 1000000u;
  s_ready = true;
}

void Mcp23s08_end(void)
{
  if (!s_ready)
    return;
  digitalWrite(s_mcpCs, HIGH);
  digitalWrite(s_cs5940, HIGH);
  digitalWrite(s_csAds1220, HIGH);
}

void Mcp23s08_writeReg(uint8_t reg, uint8_t val)
{
  if (!s_ready)
    return;
  digitalWrite(s_cs5940, HIGH);
  digitalWrite(s_csAds1220, HIGH);
  digitalWrite(s_mcpCs, LOW);
  SPI.beginTransaction(SPISettings(s_spiHz, MSBFIRST, SPI_MODE0));
  SPI.transfer(kOpWrite);
  SPI.transfer(reg);
  SPI.transfer(val);
  SPI.endTransaction();
  digitalWrite(s_mcpCs, HIGH);
}

void Mcp23s08_initOutputsAll(void)
{
  Mcp23s08_writeReg(kRegIODIR, 0x00u);
}

void Mcp23s08_setOutput(uint8_t pattern)
{
  Mcp23s08_writeReg(kRegOLAT, pattern);
}

void Mcp23s08_testPortWalk(unsigned rounds, uint32_t delayMs)
{
  Mcp23s08_initOutputsAll();
  Mcp23s08_setOutput(0x00u);
  for (unsigned r = 0; r < rounds; r++)
  {
    for (int bit = 0; bit < 8; bit++)
    {
      ESP_LOGI(TAG, "GP%d only LOW ", bit);
      Mcp23s08_setOutput(1 << bit);
      delay(delayMs);
      ESP_LOGI(TAG, "all HIGH");
      Mcp23s08_setOutput(0x00u);
      delay(delayMs);
    }
  }
}
