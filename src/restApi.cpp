#include "restApi.h"

#ifdef WIFI_AP_MODE

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_log.h>
#include <sys/time.h>
#include <time.h>

#include "mainGrobal.h"

static const char *const TAG = "RestApi";

/** 상위 시스템 JSON data[] 슬롯 수 (전압 15 + 내부저항 15). */
#define REST_API_DATA_SLOTS 30u
#define REST_API_HALF_SLOTS 15u

static WebServer s_server(80);

extern _cell_value cellvalue[MAX_INSTALLED_CELLS];
extern nvsSystemSet systemDefaultValue;

static uint16_t apiInstalledCells(void)
{
  uint16_t n = systemDefaultValue.installed_cells;
  if (n < 1)
    n = 1;
  if (n > REST_API_HALF_SLOTS)
    n = REST_API_HALF_SLOTS;
  return n;
}

static uint16_t apiVoltageMv(uint16_t cellIdx)
{
  const float v = cellvalue[cellIdx].voltage;
  if (v <= 0.0f)
    return 0;
  const float mv = v * 1000.0f + 0.5f;
  if (mv > 65535.0f)
    return 65535u;
  return (uint16_t)mv;
}

static uint16_t apiImpedanceReg(uint16_t cellIdx)
{
  const float z = cellvalue[cellIdx].impendance;
  if (z <= 0.0f)
    return 0;
  const float scaled = z * 100.0f + 0.5f;
  if (scaled > 65535.0f)
    return 65535u;
  return (uint16_t)scaled;
}

static bool apiDeviceHasValidCells(uint16_t nCells)
{
  for (uint16_t i = 0; i < nCells; i++)
  {
    if (cellvalue[i].voltage >= 0.6f)
      return true;
  }
  return false;
}

static void formatIso8601Utc(char *out, size_t cap)
{
  struct timeval tv;
  struct tm tmUtc;
  gettimeofday(&tv, NULL);
  gmtime_r(&tv.tv_sec, &tmUtc);
  snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03luZ",
           tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
           tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec,
           (unsigned long)(tv.tv_usec / 1000UL));
}

static int appendDataArray(char *buf, size_t cap, int off, const uint16_t *data, unsigned count)
{
  off += snprintf(buf + off, cap - (size_t)off, "\"data\":[");
  for (unsigned i = 0; i < count; i++)
  {
    if (i > 0)
      off += snprintf(buf + off, cap - (size_t)off, ",");
    off += snprintf(buf + off, cap - (size_t)off, "%u", (unsigned)data[i]);
  }
  off += snprintf(buf + off, cap - (size_t)off, "]");
  return off;
}

static void handleApiBattery(void)
{
  const uint16_t nCells = apiInstalledCells();
  const bool ok = apiDeviceHasValidCells(nCells);

  uint16_t samples[REST_API_DATA_SLOTS] = {0};
  for (uint16_t i = 0; i < nCells; i++)
    samples[i] = apiVoltageMv(i);
  for (uint16_t i = 0; i < nCells; i++)
    samples[REST_API_HALF_SLOTS + i] = apiImpedanceReg(i);

  char ts[32];
  formatIso8601Utc(ts, sizeof(ts));

  const float highV = systemDefaultValue.alarmHighCellVoltage / 1000.0f;
  const float lowV = systemDefaultValue.alarmLowCellVoltage / 1000.0f;

  static char json[4096];
  int off = 0;

  off += snprintf(json + off, sizeof(json) - (size_t)off,
                  "{\"status\":\"success\",\"timestamp\":\"%s\",\"data\":{\"multi_data\":{"
                  "\"devices\":{\"1\":{",
                  ts);

  if (ok)
  {
    off += snprintf(json + off, sizeof(json) - (size_t)off, "\"status\":\"success\",\"data\":{");
    off = appendDataArray(json, sizeof(json), off, samples, REST_API_DATA_SLOTS);
    off += snprintf(json + off, sizeof(json) - (size_t)off, "}}");
  }
  else
  {
    off += snprintf(json + off, sizeof(json) - (size_t)off,
                    "\"status\":\"failed\",\"data\":{");
    off = appendDataArray(json, sizeof(json), off, samples, REST_API_DATA_SLOTS);
    off += snprintf(json + off, sizeof(json) - (size_t)off,
                    "},\"error\":\"No valid cell data\"}");
  }

  off += snprintf(json + off, sizeof(json) - (size_t)off,
                  "},\"summary\":{\"total\":1,\"success\":%u,\"failed\":%u",
                  ok ? 1u : 0u, ok ? 0u : 1u);

  if (!ok)
  {
    off += snprintf(json + off, sizeof(json) - (size_t)off,
                    ",\"failedDevices\":[{\"id\":1,\"error\":\"No valid cell data\"}]");
  }

  off += snprintf(json + off, sizeof(json) - (size_t)off,
                  "}},\"rackInfo\":{"
                  "\"rackno\":%u,"
                  "\"installedmodule\":1,"
                  "\"totalbatno\":%u,"
                  "\"rackname\":\"아이에프텍(주)\","
                  "\"installdate\":\"2024-10-28T15:00:00.000Z\","
                  "\"expiredate\":\"2034-10-27T15:00:00.000Z\","
                  "\"bat_type\":\"ni-cd\","
                  "\"nominalvoltage\":1.2,"
                  "\"highvoltage\":%.3f,"
                  "\"lowvoltage\":%.3f,"
                  "\"hightemperature\":%u,"
                  "\"highimpedance\":10,"
                  "\"location\":\"주전산실\""
                  "}}}",
                  (unsigned)systemDefaultValue.modbusId,
                  (unsigned)systemDefaultValue.installed_cells,
                  highV, lowV,
                  (unsigned)systemDefaultValue.AlarmTemperature);

  s_server.sendHeader("Access-Control-Allow-Origin", "*");
  s_server.send(200, "application/json", json);
}

void restApiInit(void)
{
  s_server.on("/api/battery", HTTP_GET, handleApiBattery);
  s_server.onNotFound([]()
                      {
    s_server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"Not found\"}");
  });
  s_server.begin();
  ESP_LOGI(TAG, "GET http://%s/api/battery", WiFi.softAPIP().toString().c_str());
}

void restApiHandle(void)
{
  s_server.handleClient();
}

#endif
