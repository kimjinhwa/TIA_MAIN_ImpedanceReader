#include "restApi.h"

#ifdef WIFI_AP_MODE

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include <EEPROM.h>

#include "dataSync.h"
#include "eepromNvs.hpp"
#include "fileSystem.h"
#include "mainGrobal.h"
#include "modbusRtu.h"
#include "webFileUploadHtml.h"
#include "webJqueryMinJs.h"

static const char *const TAG = "RestApi";

/** 상위 시스템 JSON data[] 슬롯 수 (전압 15 + 내부저항 15). */
#define REST_API_DATA_SLOTS 30u
#define REST_API_HALF_SLOTS 15u

static WebServer s_server(80);
static volatile bool s_uploadInProgress = false;

static bool apiRequireSession(void);
static void sendJsonDocumentResponse(int statusCode, JsonDocument &doc);
static void sendJsonError(int statusCode, const char *errorText);

extern _cell_value cellvalue[MAX_INSTALLED_CELLS];
extern nvsSystemSet systemDefaultValue;
extern LittleFileSystem lsFile;
extern bool runRcalCalibrationAndOptionallySave(bool saveToEeprom, float *outReal, float *outImage, float *outMagnitude);
extern void AD5940_UseStoredRcalFromEeprom(void);
extern float bootRcalVerifyRealGet(void);
extern float bootRcalVerifyImageGet(void);
extern float bootRcalVerifyMagnitudeGet(void);

#define SESSION_COOKIE_NAME "session"
#define SESSION_MAX_AGE_SEC (7u * 24u * 60u * 60u)
#define BMS_IMP_PERIOD_DEFAULT_MIN 60u
#define BMS_EEPROM_CHANGE_DEFAULT_PERCENT 3u
#define BMS_IMP_STABLE_TOL_DEFAULT_PERCENT 3u
#define BMS_IMP_POST_SAMPLES_DEFAULT 5u
#define BMS_IMP_POST_SAMPLES_MAX 20u
#define BMS_IMP_MIN_VALID_DEFAULT_DECI 50u
#define BMS_IMP_READ_MAX_DEFAULT 60u
#define BMS_IMP_READ_MAX_MAX 120u
#define BMS_IMP_STABLE_WINDOW_DEFAULT 5u
#define BMS_IMP_STABLE_WINDOW_MAX 20u
#define BMS_IMP_AUTO_UPDATE_DEFAULT 0u
#define BMS_IMP_GAIN_DEFAULT_PERMILLE 1000u
#define BMS_IMP_GAIN_MIN_PERMILLE 100u
#define BMS_IMP_GAIN_MAX_PERMILLE 4000u
/** 개발 단계: 인증 전체 비활성. 배포 전 true로 복구. */
static const bool API_REQUIRE_AUTH = false;

static String g_sessionToken;
/** millis 기준 만료 — NTP와 무관 */
static uint32_t s_sessionExpireMs = 0;

static void sendCorsHeaders(void)
{
  String origin = "*";
  if (s_server.hasHeader("Origin"))
  {
    origin = s_server.header("Origin");
    origin.trim();
    if (origin.length() == 0)
      origin = "*";
  }

  if (origin != "*")
  {
    s_server.sendHeader("Access-Control-Allow-Origin", origin);
    s_server.sendHeader("Access-Control-Allow-Credentials", "true");
    s_server.sendHeader("Vary", "Origin");
  }
  else
  {
    s_server.sendHeader("Access-Control-Allow-Origin", "*");
  }
  s_server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Cookie");
  s_server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

static void sendApiPreflight(void)
{
  sendCorsHeaders();
  s_server.send(200, "text/plain", "OK");
}

static void sendJsonDocumentResponse(int statusCode, JsonDocument &doc)
{
  String payload;
  serializeJson(doc, payload);
  s_server.send(statusCode, "application/json", payload);
}

static void sendJsonError(int statusCode, const char *errorText)
{
  JsonDocument doc;
  doc["ok"] = false;
  if (errorText && errorText[0] != '\0')
    doc["error"] = errorText;
  sendJsonDocumentResponse(statusCode, doc);
}

static void handleApiHealth(void)
{
  sendCorsHeaders();
  JsonDocument doc;
  doc["ok"] = true;
  doc["service"] = "impedance-web";
  sendJsonDocumentResponse(200, doc);
}

static String makeSessionToken(void)
{
  char token[33];
  snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random());
  return String(token);
}

/** Esp32SNMPforSUN main.cpp parseCookieValue 와 동일 */
static String parseCookieValue(const String &cookieHeader, const char *name)
{
  if (cookieHeader.length() == 0 || !name)
    return "";
  const String needle = String(name) + "=";
  int start = 0;
  while (start < (int)cookieHeader.length())
  {
    int end = cookieHeader.indexOf(';', start);
    if (end < 0)
      end = cookieHeader.length();
    String part = cookieHeader.substring(start, end);
    part.trim();
    if (part.startsWith(needle))
      return part.substring(needle.length());
    start = end + 1;
  }
  return "";
}

static String requestSessionCookieValue(void)
{
  if (!s_server.hasHeader("Cookie"))
    return "";
  return parseCookieValue(s_server.header("Cookie"), SESSION_COOKIE_NAME);
}

static bool sessionIsValid(void)
{
  if (g_sessionToken.length() == 0)
    return false;
  const uint32_t nowMs = millis();
  if ((int32_t)(s_sessionExpireMs - nowMs) <= 0)
  {
    g_sessionToken = "";
    s_sessionExpireMs = 0;
    return false;
  }
  const String requestToken = requestSessionCookieValue();
  if (requestToken.length() == 0)
    return false;
  return requestToken == g_sessionToken;
}

static void sessionStart(void)
{
  g_sessionToken = makeSessionToken();
  s_sessionExpireMs = millis() + (SESSION_MAX_AGE_SEC * 1000UL);
}

static void sessionClear(void)
{
  g_sessionToken = "";
  s_sessionExpireMs = 0;
}

static void setSessionCookie(void)
{
  time_t nowEpoch = time(nullptr);
  if (nowEpoch < 0)
    nowEpoch = 0;
  const time_t expiresEpoch = nowEpoch + (time_t)SESSION_MAX_AGE_SEC;
  struct tm tmUtc;
  char expiresDate[40] = {0};
  if (gmtime_r(&expiresEpoch, &tmUtc) != nullptr)
    strftime(expiresDate, sizeof(expiresDate), "%a, %d %b %Y %H:%M:%S GMT", &tmUtc);
  else
    strncpy(expiresDate, "Wed, 01 Jan 2031 00:00:00 GMT", sizeof(expiresDate) - 1);

  const String cookie = String(SESSION_COOKIE_NAME) + "=" + g_sessionToken +
                        "; Expires=" + String(expiresDate) +
                        "; Max-Age=" + String((unsigned)SESSION_MAX_AGE_SEC) +
                        "; Path=/; HttpOnly; SameSite=Lax";
  s_server.sendHeader("Set-Cookie", cookie);
}

static void clearSessionCookie(void)
{
  s_server.sendHeader("Set-Cookie",
                      SESSION_COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
}

static void trimInPlace(char *s)
{
  if (!s)
    return;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
    s[--n] = '\0';
  size_t start = 0;
  while (s[start] == ' ' || s[start] == '\t')
    start++;
  if (start > 0)
    memmove(s, s + start, strlen(s + start) + 1);
}

static bool credentialsMatch(const char *userid, const char *passwd)
{
  if (!userid || !passwd)
    return false;

  char storedUser[sizeof(systemDefaultValue.userid) + 1] = {0};
  char storedPass[sizeof(systemDefaultValue.userpassword) + 1] = {0};
  strncpy(storedUser, systemDefaultValue.userid, sizeof(storedUser) - 1);
  strncpy(storedPass, systemDefaultValue.userpassword, sizeof(storedPass) - 1);
  trimInPlace(storedUser);
  trimInPlace(storedPass);

  return (strcmp(userid, storedUser) == 0) && (strcmp(passwd, storedPass) == 0);
}

static void handleApiLogin(void)
{
  sendCorsHeaders();

  if (s_server.method() != HTTP_POST)
  {
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = "Method not allowed";
    sendJsonDocumentResponse(405, doc);
    return;
  }

  char userid[16] = {0};
  char passwd[16] = {0};
  bool hasUser = false;
  bool hasPass = false;

  if (s_server.hasArg("plain"))
  {
    JsonDocument reqDoc;
    if (deserializeJson(reqDoc, s_server.arg("plain")) == DeserializationError::Ok)
    {
      if (!reqDoc["userid"].isNull())
      {
        const char *value = reqDoc["userid"].as<const char *>();
        if (value)
        {
          strncpy(userid, value, sizeof(userid) - 1);
          hasUser = true;
        }
      }
      if (!reqDoc["passwd"].isNull())
      {
        const char *value = reqDoc["passwd"].as<const char *>();
        if (value)
        {
          strncpy(passwd, value, sizeof(passwd) - 1);
          hasPass = true;
        }
      }
    }
  }

  if (!hasUser && s_server.hasArg("userid"))
  {
    strncpy(userid, s_server.arg("userid").c_str(), sizeof(userid) - 1);
    hasUser = userid[0] != '\0';
  }
  if (!hasPass && s_server.hasArg("passwd"))
  {
    strncpy(passwd, s_server.arg("passwd").c_str(), sizeof(passwd) - 1);
    hasPass = passwd[0] != '\0';
  }

  trimInPlace(userid);
  trimInPlace(passwd);

  if (userid[0] == '\0' || passwd[0] == '\0')
  {
    sendJsonError(400, "missing credentials");
    return;
  }

  if (!credentialsMatch(userid, passwd))
  {
    ESP_LOGW(TAG, "login failed user='%s'", userid);
    sendJsonError(401, "invalid credentials");
    return;
  }

  sessionStart();
  setSessionCookie();
  ESP_LOGI(TAG, "login ok user='%s' tokenLen=%u expMs=%lu",
           userid, (unsigned)g_sessionToken.length(), (unsigned long)s_sessionExpireMs);
  s_server.sendHeader("Connection", "close");
  JsonDocument doc;
  doc["ok"] = true;
  sendJsonDocumentResponse(200, doc);
}

static void handleApiMe(void)
{
  sendCorsHeaders();

  if (!API_REQUIRE_AUTH)
  {
    JsonDocument doc;
    doc["ok"] = true;
    doc["authenticated"] = true;
    doc["devAuthBypass"] = true;
    sendJsonDocumentResponse(200, doc);
    return;
  }

  if (!sessionIsValid())
  {
    const String reqTok = requestSessionCookieValue();
    ESP_LOGW(TAG, "/api/me denied cookieLen=%u srvLen=%u expIn=%ld",
             (unsigned)reqTok.length(), (unsigned)g_sessionToken.length(),
             (long)((int32_t)s_sessionExpireMs - (int32_t)millis()));
    JsonDocument doc;
    doc["ok"] = false;
    doc["authenticated"] = false;
    sendJsonDocumentResponse(401, doc);
    return;
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["authenticated"] = true;
  sendJsonDocumentResponse(200, doc);
}

static bool apiRequireSession(void)
{
  if (!API_REQUIRE_AUTH)
    return true;
  if (sessionIsValid())
    return true;
  sendCorsHeaders();
  JsonDocument doc;
  doc["ok"] = false;
  doc["authenticated"] = false;
  sendJsonDocumentResponse(401, doc);
  return false;
}

static String ipUint32ToString(uint32_t ip)
{
  return IPAddress(ip).toString();
}

static void copySystemConfigSnapshot(nvsSystemSet *out)
{
  if (!out)
    return;
  dataSyncLockSystemConfig();
  memcpy(out, &systemDefaultValue, sizeof(nvsSystemSet));
  dataSyncUnlockSystemConfig();
}

static void copyCfgField(char *dest, size_t destSize, const char *src)
{
  if (!dest || destSize == 0 || !src || src[0] == '\0')
    return;
  strncpy(dest, src, destSize - 1);
  dest[destSize - 1] = '\0';
}

static void handleApiNetworkConfigGet(void)
{
  sendCorsHeaders();
  nvsSystemSet cfg;
  copySystemConfigSnapshot(&cfg);

  JsonDocument doc;
  JsonObject network = doc["network"].to<JsonObject>();
  network["macAddress"] = WiFi.macAddress();
  network["ipAddress"] = ipUint32ToString(cfg.IPADDRESS);
  network["subnetMask"] = ipUint32ToString(cfg.SUBNETMASK);
  network["gateway"] = ipUint32ToString(cfg.GATEWAY);

  JsonObject snmpAccess = doc["snmpAccess"].to<JsonObject>();
  snmpAccess["ipAddress"] = "0.0.0.0";
  snmpAccess["community"] = "public";
  snmpAccess["permission"] = "NOACCESS";

  JsonObject trapAccess = doc["trapAccess"].to<JsonObject>();
  trapAccess["ipAddress"] = "0.0.0.0";
  trapAccess["community"] = "public";
  trapAccess["accept"] = true;

  JsonObject webAccess = doc["webAccess"].to<JsonObject>();
  webAccess["webPort"] = 80;
  webAccess["accessIp1"] = "0.0.0.0";
  webAccess["accessIp2"] = "0.0.0.0";
  webAccess["id"] = cfg.userid;
  webAccess["pw"] = "";

  sendJsonDocumentResponse(200, doc);
}

static void handleApiNetworkConfigPost(void)
{
  sendCorsHeaders();

  if (!s_server.hasArg("plain"))
  {
    sendJsonError(400, "missing body");
    return;
  }

  JsonDocument reqDoc;
  if (deserializeJson(reqDoc, s_server.arg("plain")) != DeserializationError::Ok)
  {
    sendJsonError(400, "invalid json body");
    return;
  }

  JsonObject network = reqDoc["network"].as<JsonObject>();
  JsonObject webAccess = reqDoc["webAccess"].as<JsonObject>();
  const char *ipText = network["ipAddress"] | "";
  const char *subnetText = network["subnetMask"] | "";
  const char *gatewayText = network["gateway"] | "";
  const char *webId = webAccess["id"] | "";
  const char *webPw = webAccess["pw"] | "";
  bool hasNetworkIp = !network["ipAddress"].isNull() || !network["subnetMask"].isNull() || !network["gateway"].isNull();
  bool hasWebAccess = !webAccess["id"].isNull() || !webAccess["pw"].isNull();

  IPAddress newIp;
  IPAddress newSubnet;
  IPAddress newGateway;
  if (ipText[0] && subnetText[0] && gatewayText[0] &&
      !(newIp.fromString(ipText) && newSubnet.fromString(subnetText) && newGateway.fromString(gatewayText)))
  {
    sendJsonError(400, "invalid network ip");
    return;
  }
  dataSyncLockSystemConfig();
  if (hasNetworkIp && ipText[0] && newIp.fromString(ipText))
    systemDefaultValue.IPADDRESS = static_cast<uint32_t>(newIp);
  if (hasNetworkIp && subnetText[0] && newSubnet.fromString(subnetText))
    systemDefaultValue.SUBNETMASK = static_cast<uint32_t>(newSubnet);
  if (hasNetworkIp && gatewayText[0] && newGateway.fromString(gatewayText))
    systemDefaultValue.GATEWAY = static_cast<uint32_t>(newGateway);

  if (hasWebAccess)
  {
    char webIdBuf[16] = {0};
    char webPwBuf[16] = {0};
    strncpy(webIdBuf, webId, sizeof(webIdBuf) - 1);
    strncpy(webPwBuf, webPw, sizeof(webPwBuf) - 1);
    trimInPlace(webIdBuf);
    trimInPlace(webPwBuf);
    if (webIdBuf[0])
      copyCfgField(systemDefaultValue.userid, sizeof(systemDefaultValue.userid), webIdBuf);
    if (webPwBuf[0])
      copyCfgField(systemDefaultValue.userpassword, sizeof(systemDefaultValue.userpassword), webPwBuf);
  }

  const bool saved = readnWriteEEProm(true);
  char userLog[sizeof(systemDefaultValue.userid) + 1] = {0};
  strncpy(userLog, systemDefaultValue.userid, sizeof(userLog) - 1);
  dataSyncUnlockSystemConfig();

  if (!saved)
  {
    JsonDocument doc;
    doc["ok"] = false;
    doc["rebootRequired"] = false;
    sendJsonDocumentResponse(500, doc);
    return;
  }

  ESP_LOGI(TAG, "network-config saved ip=%s user=%s", ipText, userLog);
  s_server.sendHeader("Connection", "close");
  JsonDocument doc;
  doc["ok"] = true;
  doc["rebootRequired"] = true;
  sendJsonDocumentResponse(200, doc);
  delay(800);
  ESP.restart();
}

static uint16_t bmsSanitizeImpedancePeriodMin(uint32_t min)
{
  if (min == 0)
    return (uint16_t)BMS_IMP_PERIOD_DEFAULT_MIN;
  if (min > 65535u)
    return 65535u;
  return (uint16_t)min;
}

static uint16_t bmsSanitizeImpedanceReadMax(uint32_t value)
{
  if (value < 1u || value > BMS_IMP_READ_MAX_MAX)
    return (uint16_t)BMS_IMP_READ_MAX_DEFAULT;
  return (uint16_t)value;
}

static uint16_t bmsSanitizeImpedanceStableWindow(uint32_t value, uint16_t readMax)
{
  uint16_t v = (uint16_t)value;
  if (value < 2u || value > BMS_IMP_STABLE_WINDOW_MAX)
    v = (uint16_t)BMS_IMP_STABLE_WINDOW_DEFAULT;
  if (v > readMax)
    v = readMax;
  if (v < 2u)
    v = 2u;
  return v;
}

static uint8_t bmsSanitizeImpedanceEepromChangePercent(uint32_t percent)
{
  if (percent < 1u || percent > 100u)
    return (uint8_t)BMS_EEPROM_CHANGE_DEFAULT_PERCENT;
  return (uint8_t)percent;
}

static uint8_t bmsSanitizeImpedanceStableTolPercent(uint32_t percent)
{
  if (percent < 1u || percent > 20u)
    return (uint8_t)BMS_IMP_STABLE_TOL_DEFAULT_PERCENT;
  return (uint8_t)percent;
}

static uint8_t bmsSanitizeImpedancePostSamples(uint32_t samples)
{
  if (samples < 1u || samples > BMS_IMP_POST_SAMPLES_MAX)
    return (uint8_t)BMS_IMP_POST_SAMPLES_DEFAULT;
  return (uint8_t)samples;
}

static uint16_t bmsSanitizeImpedanceMinValidDeciMohm(uint32_t deciMohm)
{
  if (deciMohm < 1u || deciMohm > 10000u)
    return (uint16_t)BMS_IMP_MIN_VALID_DEFAULT_DECI;
  return (uint16_t)deciMohm;
}

static uint16_t bmsSanitizeImpedanceGainPermille(uint32_t permille)
{
  if (permille < BMS_IMP_GAIN_MIN_PERMILLE || permille > BMS_IMP_GAIN_MAX_PERMILLE)
    return (uint16_t)BMS_IMP_GAIN_DEFAULT_PERMILLE;
  return (uint16_t)permille;
}

static int16_t impedanceCompCentiFromMohm(float mohms)
{
  float scaled = mohms * 100.0f;
  if (scaled > 32767.0f)
    scaled = 32767.0f;
  if (scaled < -32768.0f)
    scaled = -32768.0f;
  return (int16_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
}

static float impedanceCompMohmFromCenti(int16_t centi)
{
  return (float)centi / 100.0f;
}

static float removeGlobalCalibrationFromMohm(float adjustedMohm, float gain, float offsetMohm)
{
  if (gain < 0.0001f)
    gain = 1.0f;
  const float raw = (adjustedMohm - offsetMohm) / gain;
  return raw > 0.0f ? raw : 0.0f;
}

static float applyGlobalCalibrationToMohm(float rawMohm, float gain, float offsetMohm)
{
  const float adjusted = rawMohm * gain + offsetMohm;
  return adjusted > 0.0f ? adjusted : 0.0f;
}

static float rcalMagnitudeMohm(float real, float image)
{
  return sqrtf(real * real + image * image);
}

static void applySystemDefaultsLocked(nvsSystemSet *cfg)
{
  if (!cfg)
    return;
  memset(cfg, 0, sizeof(*cfg));

  cfg->runMode = 0;
  cfg->AlarmAmpere = 2000;
  cfg->alarmDiffCellVoltage = 200;
  cfg->alarmHighCellVoltage = 1450;
  cfg->alarmLowCellVoltage = 850;
  cfg->AlarmTemperature = 65;
  cfg->cutoffHighCellVoltage = 14800;
  cfg->cutoffLowCellVoltage = 6500;
  cfg->GATEWAY = (uint32_t)IPAddress(192, 168, 0, 1);
  cfg->IPADDRESS = (uint32_t)IPAddress(192, 168, 0, 201);
  cfg->SUBNETMASK = (uint32_t)IPAddress(255, 255, 255, 0);
  cfg->modbusId = 1;
  cfg->installed_cells = 15;
  cfg->startBatnumber = 1;
  cfg->real_Cal = -33410.0f;
  cfg->image_Cal = 35511.0f;
  cfg->logLevel = ESP_LOG_INFO;

  strncpy(cfg->ssid, "iptime_mbhong", sizeof(cfg->ssid) - 1);
  strncpy(cfg->userid, "admin", sizeof(cfg->userid) - 1);
  strncpy(cfg->userpassword, "admin", sizeof(cfg->userpassword) - 1);

  for (int i = 0; i < MAX_INSTALLED_CELLS; i++)
  {
    cfg->voltageCompensation[i] = 0;
    cfg->impendanceCompensation[i] = 0;
    cfg->baseVoltage[i] = 0;
    cfg->baseImpendance[i] = 0;
  }

  cfg->ImpedanceMeasurePeriod = (uint16_t)BMS_IMP_PERIOD_DEFAULT_MIN;
  cfg->ImpedanceFactor = (uint8_t)BMS_EEPROM_CHANGE_DEFAULT_PERCENT;
  cfg->ACVoltPP = (uint16_t)BMS_IMP_READ_MAX_DEFAULT;
  cfg->DCVolt = (uint16_t)BMS_IMP_STABLE_WINDOW_DEFAULT;
  cfg->VoltageFactor = (uint8_t)BMS_IMP_STABLE_TOL_DEFAULT_PERCENT;
  cfg->TemperatureFactor = (uint8_t)BMS_IMP_POST_SAMPLES_DEFAULT;
  cfg->RcalLoopCount = (uint16_t)BMS_IMP_MIN_VALID_DEFAULT_DECI;
  cfg->impedanceAutoUpdateEnabled = (uint8_t)BMS_IMP_AUTO_UPDATE_DEFAULT;
  cfg->impedanceGainPermille = (uint16_t)BMS_IMP_GAIN_DEFAULT_PERMILLE;
  cfg->impedanceOffsetCentiMohm = 0;
}

static void handleApiRcalCalibrationPost(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  bool saveToEeprom = false;
  if (s_server.hasArg("plain"))
  {
    JsonDocument reqDoc;
    if (deserializeJson(reqDoc, s_server.arg("plain")) == DeserializationError::Ok)
      saveToEeprom = reqDoc["save"] | false;
  }

  float real = 0.0f, image = 0.0f, magnitude = 0.0f;
  const bool ok = runRcalCalibrationAndOptionallySave(saveToEeprom, &real, &image, &magnitude);
  if (!ok)
  {
    sendJsonError(400, "rcal calibration failed");
    return;
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["real"] = real;
  doc["image"] = image;
  doc["magnitudeMohm"] = magnitude;
  doc["saved"] = saveToEeprom;
  sendJsonDocumentResponse(200, doc);
}

static void handleApiImpedanceCompensationGet(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  nvsSystemSet cfg;
  copySystemConfigSnapshot(&cfg);
  uint16_t nCells = cfg.installed_cells;
  if (nCells < 1)
    nCells = 1;
  if (nCells > MAX_INSTALLED_CELLS)
    nCells = MAX_INSTALLED_CELLS;

  JsonDocument doc;
  doc["ok"] = true;
  JsonObject global = doc["global"].to<JsonObject>();
  global["impedanceGain"] = (float)bmsSanitizeImpedanceGainPermille(cfg.impedanceGainPermille) / 1000.0f;
  global["impedanceOffsetMohm"] = (float)cfg.impedanceOffsetCentiMohm / 100.0f;

  JsonArray cells = doc["cells"].to<JsonArray>();
  for (uint16_t i = 0; i < nCells; i++)
  {
    JsonObject item = cells.add<JsonObject>();
    item["cellNo"] = (unsigned)(i + 1);
    item["currentMohm"] = cellvalue[i].impendance;
    item["baseMohm"] = impedanceCompMohmFromCenti(cfg.baseImpendance[i]);
    item["compensationMohm"] = impedanceCompMohmFromCenti(cfg.impendanceCompensation[i]);
  }
  sendJsonDocumentResponse(200, doc);
}

static void handleApiImpedanceCompensationPost(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  if (!s_server.hasArg("plain"))
  {
    sendJsonError(400, "missing body");
    return;
  }

  JsonDocument reqDoc;
  if (deserializeJson(reqDoc, s_server.arg("plain")) != DeserializationError::Ok)
  {
    sendJsonError(400, "invalid json body");
    return;
  }

  const int cellNo = reqDoc["cellNo"] | -1;
  const String op = reqDoc["op"] | "set";

  if (op == "setBase")
  {
    const float baseMohm = reqDoc["baseMohm"] | NAN;
    if (cellNo < 1 || cellNo > MAX_INSTALLED_CELLS)
    {
      sendJsonError(400, "cellNo out of range(1..20)");
      return;
    }
    if (!isfinite(baseMohm) || baseMohm < 0.0f || baseMohm > 327.67f)
    {
      sendJsonError(400, "baseMohm out of range(0.00..327.67)");
      return;
    }

    const int16_t baseCenti = impedanceCompCentiFromMohm(baseMohm);
    dataSyncLockSystemConfig();
    systemDefaultValue.baseImpendance[cellNo - 1] = baseCenti;
    const bool saved = readnWriteEEProm(true);
    dataSyncUnlockSystemConfig();
    if (!saved)
    {
      sendJsonError(500, "eeprom commit failed");
      return;
    }

    cellvalue[cellNo - 1].baseImpendance = baseCenti;

    JsonDocument doc;
    doc["ok"] = true;
    doc["cellNo"] = cellNo;
    doc["baseMohm"] = impedanceCompMohmFromCenti(baseCenti);
    sendJsonDocumentResponse(200, doc);
    return;
  }

  const float compMohm = reqDoc["compensationMohm"] | NAN;
  if (cellNo < 0 || cellNo > MAX_INSTALLED_CELLS)
  {
    sendJsonError(400, "cellNo out of range(0..20)");
    return;
  }
  if (!isfinite(compMohm) || compMohm < -327.68f || compMohm > 327.67f)
  {
    sendJsonError(400, "compensationMohm out of range(-327.68..327.67)");
    return;
  }

  const int16_t compCenti = impedanceCompCentiFromMohm(compMohm);
  int16_t oldCompCenti[MAX_INSTALLED_CELLS] = {0};
  dataSyncLockSystemConfig();
  if (cellNo == 0)
  {
    for (int i = 0; i < MAX_INSTALLED_CELLS; i++)
    {
      oldCompCenti[i] = systemDefaultValue.impendanceCompensation[i];
      systemDefaultValue.impendanceCompensation[i] = compCenti;
    }
  }
  else
  {
    oldCompCenti[cellNo - 1] = systemDefaultValue.impendanceCompensation[cellNo - 1];
    systemDefaultValue.impendanceCompensation[cellNo - 1] = compCenti;
  }
  const bool saved = readnWriteEEProm(true);
  dataSyncUnlockSystemConfig();
  if (!saved)
  {
    sendJsonError(500, "eeprom commit failed");
    return;
  }

  if (cellNo == 0)
  {
    for (int i = 0; i < MAX_INSTALLED_CELLS; i++)
    {
      const float deltaMohm = impedanceCompMohmFromCenti((int16_t)(compCenti - oldCompCenti[i]));
      const float zNow = cellvalue[i].impendance;
      if (zNow > 0.0f)
      {
        const float zAdjusted = zNow + deltaMohm;
        cellvalue[i].impendance = zAdjusted > 0.0f ? zAdjusted : 0.0f;
      }
      cellvalue[i].impendanceCompensation = compCenti;
    }
  }
  else
  {
    const int idx = cellNo - 1;
    const float deltaMohm = impedanceCompMohmFromCenti((int16_t)(compCenti - oldCompCenti[idx]));
    const float zNow = cellvalue[idx].impendance;
    if (zNow > 0.0f)
    {
      const float zAdjusted = zNow + deltaMohm;
      cellvalue[idx].impendance = zAdjusted > 0.0f ? zAdjusted : 0.0f;
    }
    cellvalue[idx].impendanceCompensation = compCenti;
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["cellNo"] = cellNo;
  doc["compensationMohm"] = impedanceCompMohmFromCenti(compCenti);
  if (cellNo > 0)
    doc["currentMohm"] = cellvalue[cellNo - 1].impendance;
  sendJsonDocumentResponse(200, doc);
}

static void handleApiSystemActionPost(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  if (!s_server.hasArg("plain"))
  {
    sendJsonError(400, "missing body");
    return;
  }

  char action[40] = {0};
  bool hasAction = false;

  JsonDocument reqDoc;
  if (deserializeJson(reqDoc, s_server.arg("plain")) == DeserializationError::Ok)
  {
    const char *actionField = reqDoc["action"] | nullptr;
    if (actionField && actionField[0] != '\0')
    {
      strncpy(action, actionField, sizeof(action) - 1);
      hasAction = true;
    }
  }
  if (!hasAction && s_server.hasArg("action"))
  {
    const String actionArg = s_server.arg("action");
    strncpy(action, actionArg.c_str(), sizeof(action) - 1);
    hasAction = action[0] != '\0';
  }
  if (!hasAction)
  {
    sendJsonError(400, "missing action");
    return;
  }

  if (strcmp(action, "formatFsFast") == 0)
  {
    lsFile.littleFsInitFast(1);
    JsonDocument doc;
    doc["ok"] = true;
    doc["action"] = "formatFsFast";
    doc["done"] = true;
    sendJsonDocumentResponse(200, doc);
    return;
  }

  if (strcmp(action, "resetSystemDefaults") == 0)
  {
    dataSyncLockSystemConfig();
    applySystemDefaultsLocked(&systemDefaultValue);
    const bool saved = readnWriteEEProm(true);
    dataSyncUnlockSystemConfig();
    if (!saved)
    {
      sendJsonError(500, "eeprom commit failed");
      return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["action"] = "resetSystemDefaults";
    doc["done"] = true;
    sendJsonDocumentResponse(200, doc);
    return;
  }

  if (strcmp(action, "reboot") == 0)
  {
    s_server.sendHeader("Connection", "close");
    JsonDocument doc;
    doc["ok"] = true;
    doc["action"] = "reboot";
    doc["rebooting"] = true;
    sendJsonDocumentResponse(200, doc);
    delay(600);
    ESP.restart();
    return;
  }

  sendJsonError(400, "unknown action");
}

static void handleApiImpedanceBaselineStartPost(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  if (modbusReg50BaseImpProgress != 0)
  {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "baseline scan busy";
    doc["progressCell"] = (unsigned)modbusReg50BaseImpProgress;
    sendJsonDocumentResponse(409, doc);
    return;
  }

  modbusOnFc06Reg50Write(1);
  JsonDocument doc;
  doc["ok"] = true;
  doc["started"] = true;
  doc["progressCell"] = (unsigned)modbusReg50BaseImpProgress;
  sendJsonDocumentResponse(200, doc);
}

static void handleApiImpedanceBaselineStatusGet(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  nvsSystemSet cfg;
  copySystemConfigSnapshot(&cfg);
  uint16_t totalCells = cfg.installed_cells;
  if (totalCells < 1)
    totalCells = 1;
  if (totalCells > MAX_INSTALLED_CELLS)
    totalCells = MAX_INSTALLED_CELLS;

  const uint16_t progressCell = modbusReg50BaseImpProgress;
  const bool running = (progressCell > 0 && progressCell <= totalCells);
  const uint16_t completed = running ? (uint16_t)(progressCell - 1u) : 0u;
  const uint16_t percent = running
                               ? (uint16_t)(((uint32_t)completed * 100u) / (uint32_t)totalCells)
                               : 0u;

  JsonDocument doc;
  doc["ok"] = true;
  doc["running"] = running;
  doc["progressCell"] = (unsigned)progressCell;
  doc["completedCells"] = (unsigned)completed;
  doc["totalCells"] = (unsigned)totalCells;
  doc["percent"] = (unsigned)percent;
  sendJsonDocumentResponse(200, doc);
}

static void handleApiBmsConfigGet(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  nvsSystemSet cfg;
  copySystemConfigSnapshot(&cfg);
  const uint16_t periodMin = bmsSanitizeImpedancePeriodMin(cfg.ImpedanceMeasurePeriod);
  const uint16_t readMax = bmsSanitizeImpedanceReadMax(cfg.ACVoltPP);
  const uint16_t stableWindow = bmsSanitizeImpedanceStableWindow(cfg.DCVolt, readMax);
  const uint8_t changePercent = bmsSanitizeImpedanceEepromChangePercent(cfg.ImpedanceFactor);
  const uint8_t stableTolPercent = bmsSanitizeImpedanceStableTolPercent(cfg.VoltageFactor);
  const uint8_t postSamples = bmsSanitizeImpedancePostSamples(cfg.TemperatureFactor);
  const uint16_t minValidDeci = bmsSanitizeImpedanceMinValidDeciMohm(cfg.RcalLoopCount);
  const uint16_t impGainPermille = bmsSanitizeImpedanceGainPermille(cfg.impedanceGainPermille);
  const int16_t impOffsetCenti = cfg.impedanceOffsetCentiMohm;
  const uint8_t impAutoUpdateEnabled = cfg.impedanceAutoUpdateEnabled ? 1u : 0u;

  JsonDocument doc;
  doc["ok"] = true;
  JsonObject bmsControl = doc["bmsControl"].to<JsonObject>();
  bmsControl["cellGain"] = (unsigned)modbusGetCellGain();
  bmsControl["cellOffset"] = (int)modbusGetCellOffset();
  bmsControl["useHoleCT"] = (unsigned)modbusGetUseHoleCt();
  bmsControl["ampereOffset"] = (int)modbusGetAmpereOffset();
  bmsControl["ampereGain"] = (unsigned)modbusGetAmpereGain();
  bmsControl["impedanceEepromChangePercent"] = (unsigned)changePercent;
  bmsControl["impedanceMeasurePeriodMin"] = (unsigned)periodMin;
  bmsControl["impedanceReadMax"] = (unsigned)readMax;
  bmsControl["impedanceStableWindow"] = (unsigned)stableWindow;
  bmsControl["impedanceStableTolPercent"] = (unsigned)stableTolPercent;
  bmsControl["impedancePostStableSamples"] = (unsigned)postSamples;
  bmsControl["impedanceMinValidMohm"] = (float)minValidDeci / 10.0f;
  bmsControl["impedanceAutoUpdateEnabled"] = (unsigned)impAutoUpdateEnabled;
  bmsControl["impedanceGain"] = (float)impGainPermille / 1000.0f;
  bmsControl["impedanceOffsetMohm"] = (float)impOffsetCenti / 100.0f;
  bmsControl["bootRcalReal"] = bootRcalVerifyRealGet();
  bmsControl["bootRcalImage"] = bootRcalVerifyImageGet();
  bmsControl["bootRcalMagnitudeMohm"] = bootRcalVerifyMagnitudeGet();
  bmsControl["rcalReal"] = cfg.real_Cal;
  bmsControl["rcalImage"] = cfg.image_Cal;
  bmsControl["rcalMagnitudeMohm"] = rcalMagnitudeMohm(cfg.real_Cal, cfg.image_Cal);
  sendJsonDocumentResponse(200, doc);
}

static void handleApiBmsConfigPost(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  if (!s_server.hasArg("plain"))
  {
    sendJsonError(400, "missing body");
    return;
  }

  JsonDocument reqDoc;
  if (deserializeJson(reqDoc, s_server.arg("plain")) != DeserializationError::Ok)
  {
    sendJsonError(400, "invalid json body");
    return;
  }

  JsonObject cfgObj = reqDoc["bmsControl"].is<JsonObject>() ? reqDoc["bmsControl"].as<JsonObject>() : reqDoc.as<JsonObject>();
  const bool hasPercent = !cfgObj["impedanceEepromChangePercent"].isNull();
  const bool hasPeriod = !cfgObj["impedanceMeasurePeriodMin"].isNull() ||
                         !cfgObj["impedanceMeasurePeriodSec"].isNull();
  const bool hasReadMax = !cfgObj["impedanceReadMax"].isNull();
  const bool hasStableWindow = !cfgObj["impedanceStableWindow"].isNull();
  const bool hasStableTol = !cfgObj["impedanceStableTolPercent"].isNull();
  const bool hasPostSamples = !cfgObj["impedancePostStableSamples"].isNull();
  const bool hasMinValid = !cfgObj["impedanceMinValidMohm"].isNull();
  const bool hasCellGain = !cfgObj["cellGain"].isNull();
  const bool hasCellOffset = !cfgObj["cellOffset"].isNull();
  const bool hasUseHoleCt = !cfgObj["useHoleCT"].isNull();
  const bool hasAmpOffset = !cfgObj["ampereOffset"].isNull();
  const bool hasAmpGain = !cfgObj["ampereGain"].isNull();
  const bool hasImpAutoUpdate = !cfgObj["impedanceAutoUpdateEnabled"].isNull();
  const bool hasImpGain = !cfgObj["impedanceGain"].isNull();
  const bool hasImpOffset = !cfgObj["impedanceOffsetMohm"].isNull();
  const bool hasRcalReal = !cfgObj["rcalReal"].isNull();
  const bool hasRcalImage = !cfgObj["rcalImage"].isNull();
  uint8_t nextPercent = 0;
  uint16_t nextPeriod = 0;
  uint16_t nextReadMax = 0;
  uint16_t nextStableWindow = 0;
  uint8_t nextStableTol = 0;
  uint8_t nextPostSamples = 0;
  uint16_t nextMinValidDeci = 0;
  uint16_t nextCellGain = 0;
  int16_t nextCellOffset = 0;
  uint16_t nextUseHoleCt = 0;
  int16_t nextAmpOffset = 0;
  uint16_t nextAmpGain = 0;
  uint8_t nextImpAutoUpdate = 0;
  uint16_t nextImpGainPermille = 0;
  int16_t nextImpOffsetCenti = 0;
  float nextRcalReal = 0.0f;
  float nextRcalImage = 0.0f;

  if (!hasPercent && !hasPeriod && !hasReadMax && !hasStableWindow && !hasStableTol &&
      !hasPostSamples && !hasMinValid && !hasCellGain && !hasCellOffset &&
      !hasUseHoleCt && !hasAmpOffset && !hasAmpGain &&
      !hasImpAutoUpdate &&
      !hasImpGain && !hasImpOffset &&
      !hasRcalReal && !hasRcalImage)
  {
    sendJsonError(400, "no bmsControl fields");
    return;
  }

  auto parseLongValue = [&](const char *key, long *out) -> bool
  {
    JsonVariant value = cfgObj[key];
    if (value.isNull())
      return false;
    if (value.is<long>() || value.is<int>() || value.is<unsigned int>() || value.is<float>() || value.is<double>())
    {
      *out = value.as<long>();
      return true;
    }
    const char *text = value.as<const char *>();
    if (!text)
      return false;
    char *endptr = nullptr;
    long parsed = strtol(text, &endptr, 10);
    if (!endptr || endptr == text || *endptr != '\0')
      return false;
    *out = parsed;
    return true;
  };

  auto parseFloatValue = [&](const char *key, float *out) -> bool
  {
    JsonVariant value = cfgObj[key];
    if (value.isNull())
      return false;
    if (value.is<float>() || value.is<double>() || value.is<long>() || value.is<int>() || value.is<unsigned int>())
    {
      *out = value.as<float>();
      return true;
    }
    const char *text = value.as<const char *>();
    if (!text)
      return false;
    char *endptr = nullptr;
    float parsed = strtof(text, &endptr);
    if (!endptr || endptr == text || *endptr != '\0')
      return false;
    *out = parsed;
    return true;
  };

  if (hasCellGain)
  {
    long v = 0;
    if (!parseLongValue("cellGain", &v))
    {
      sendJsonError(400, "cellGain invalid");
      return;
    }
    if (v < 1 || v > 65535)
    {
      sendJsonError(400, "cellGain out of range(1..65535)");
      return;
    }
    nextCellGain = (uint16_t)v;
  }

  if (hasCellOffset)
  {
    long v = 0;
    if (!parseLongValue("cellOffset", &v))
    {
      sendJsonError(400, "cellOffset invalid");
      return;
    }
    if (v < -32768 || v > 32767)
    {
      sendJsonError(400, "cellOffset out of range(-32768..32767)");
      return;
    }
    nextCellOffset = (int16_t)v;
  }

  if (hasUseHoleCt)
  {
    long v = 0;
    if (!parseLongValue("useHoleCT", &v))
    {
      sendJsonError(400, "useHoleCT invalid");
      return;
    }
    if (v < 0 || v > 65535)
    {
      sendJsonError(400, "useHoleCT out of range(0..65535)");
      return;
    }
    nextUseHoleCt = (uint16_t)v;
  }

  if (hasAmpOffset)
  {
    long v = 0;
    if (!parseLongValue("ampereOffset", &v))
    {
      sendJsonError(400, "ampereOffset invalid");
      return;
    }
    if (v < -32768 || v > 32767)
    {
      sendJsonError(400, "ampereOffset out of range(-32768..32767)");
      return;
    }
    nextAmpOffset = (int16_t)v;
  }

  if (hasAmpGain)
  {
    long v = 0;
    if (!parseLongValue("ampereGain", &v))
    {
      sendJsonError(400, "ampereGain invalid");
      return;
    }
    if (v < 1 || v > 65535)
    {
      sendJsonError(400, "ampereGain out of range(1..65535)");
      return;
    }
    nextAmpGain = (uint16_t)v;
  }

  if (hasPercent)
  {
    long v = 0;
    if (!parseLongValue("impedanceEepromChangePercent", &v))
    {
      sendJsonError(400, "impedanceEepromChangePercent invalid");
      return;
    }
    if (v < 1 || v > 100)
    {
      sendJsonError(400, "impedanceEepromChangePercent out of range(1..100)");
      return;
    }
    nextPercent = (uint8_t)v;
  }

  if (hasPeriod)
  {
    long v = 0;
    if (!cfgObj["impedanceMeasurePeriodMin"].isNull())
    {
      if (!parseLongValue("impedanceMeasurePeriodMin", &v))
      {
        sendJsonError(400, "impedanceMeasurePeriodMin invalid");
        return;
      }
    }
    else if (!parseLongValue("impedanceMeasurePeriodSec", &v))
    {
      sendJsonError(400, "impedanceMeasurePeriodMin invalid");
      return;
    }
    if (v < 1 || v > 65535)
    {
      sendJsonError(400, "impedanceMeasurePeriodMin out of range(1..65535)");
      return;
    }
    nextPeriod = (uint16_t)v;
  }

  if (hasReadMax)
  {
    long v = 0;
    if (!parseLongValue("impedanceReadMax", &v))
    {
      sendJsonError(400, "impedanceReadMax invalid");
      return;
    }
    if (v < 1 || v > (long)BMS_IMP_READ_MAX_MAX)
    {
      sendJsonError(400, "impedanceReadMax out of range(1..120)");
      return;
    }
    nextReadMax = (uint16_t)v;
  }

  if (hasStableWindow)
  {
    long v = 0;
    if (!parseLongValue("impedanceStableWindow", &v))
    {
      sendJsonError(400, "impedanceStableWindow invalid");
      return;
    }
    if (v < 2 || v > (long)BMS_IMP_STABLE_WINDOW_MAX)
    {
      sendJsonError(400, "impedanceStableWindow out of range(2..20)");
      return;
    }
    nextStableWindow = (uint16_t)v;
  }

  if (hasStableTol)
  {
    long v = 0;
    if (!parseLongValue("impedanceStableTolPercent", &v))
    {
      sendJsonError(400, "impedanceStableTolPercent invalid");
      return;
    }
    if (v < 1 || v > 20)
    {
      sendJsonError(400, "impedanceStableTolPercent out of range(1..20)");
      return;
    }
    nextStableTol = (uint8_t)v;
  }

  if (hasPostSamples)
  {
    long v = 0;
    if (!parseLongValue("impedancePostStableSamples", &v))
    {
      sendJsonError(400, "impedancePostStableSamples invalid");
      return;
    }
    if (v < 1 || v > (long)BMS_IMP_POST_SAMPLES_MAX)
    {
      sendJsonError(400, "impedancePostStableSamples out of range(1..20)");
      return;
    }
    nextPostSamples = (uint8_t)v;
  }

  if (hasMinValid)
  {
    float v = 0.0f;
    if (!parseFloatValue("impedanceMinValidMohm", &v))
    {
      sendJsonError(400, "impedanceMinValidMohm invalid");
      return;
    }
    if (v < 0.1f || v > 1000.0f)
    {
      sendJsonError(400, "impedanceMinValidMohm out of range(0.1..1000.0)");
      return;
    }
    nextMinValidDeci = (uint16_t)(v * 10.0f + 0.5f);
  }

  if (hasImpGain)
  {
    float v = 0.0f;
    if (!parseFloatValue("impedanceGain", &v))
    {
      sendJsonError(400, "impedanceGain invalid");
      return;
    }
    if (v < 0.1f || v > 4.0f)
    {
      sendJsonError(400, "impedanceGain out of range(0.1..4.0)");
      return;
    }
    nextImpGainPermille = (uint16_t)(v * 1000.0f + 0.5f);
  }

  if (hasImpAutoUpdate)
  {
    long v = 0;
    if (!parseLongValue("impedanceAutoUpdateEnabled", &v))
    {
      sendJsonError(400, "impedanceAutoUpdateEnabled invalid");
      return;
    }
    if (v < 0 || v > 1)
    {
      sendJsonError(400, "impedanceAutoUpdateEnabled out of range(0..1)");
      return;
    }
    nextImpAutoUpdate = (uint8_t)v;
  }

  if (hasImpOffset)
  {
    float v = 0.0f;
    if (!parseFloatValue("impedanceOffsetMohm", &v))
    {
      sendJsonError(400, "impedanceOffsetMohm invalid");
      return;
    }
    if (v < -327.68f || v > 327.67f)
    {
      sendJsonError(400, "impedanceOffsetMohm out of range(-327.68..327.67)");
      return;
    }
    nextImpOffsetCenti = (int16_t)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
  }

  if (hasRcalReal)
  {
    float v = 0.0f;
    if (!parseFloatValue("rcalReal", &v))
    {
      sendJsonError(400, "rcalReal invalid");
      return;
    }
    nextRcalReal = v;
  }

  if (hasRcalImage)
  {
    float v = 0.0f;
    if (!parseFloatValue("rcalImage", &v))
    {
      sendJsonError(400, "rcalImage invalid");
      return;
    }
    nextRcalImage = v;
  }

  dataSyncLockSystemConfig();
  const uint16_t oldImpGainPermille = bmsSanitizeImpedanceGainPermille(systemDefaultValue.impedanceGainPermille);
  const int16_t oldImpOffsetCenti = systemDefaultValue.impedanceOffsetCentiMohm;
  if (hasPercent)
    systemDefaultValue.ImpedanceFactor = nextPercent;
  if (hasPeriod)
    systemDefaultValue.ImpedanceMeasurePeriod = nextPeriod;
  if (hasReadMax)
    systemDefaultValue.ACVoltPP = nextReadMax;
  if (hasStableWindow)
    systemDefaultValue.DCVolt = nextStableWindow;
  /* readMax/stableWindow 동시 입력 시 window <= readMax 보정 */
  {
    const uint16_t readMaxNow = bmsSanitizeImpedanceReadMax(systemDefaultValue.ACVoltPP);
    systemDefaultValue.ACVoltPP = readMaxNow;
    systemDefaultValue.DCVolt = bmsSanitizeImpedanceStableWindow(systemDefaultValue.DCVolt, readMaxNow);
  }
  if (hasStableTol)
    systemDefaultValue.VoltageFactor = nextStableTol;
  if (hasPostSamples)
    systemDefaultValue.TemperatureFactor = nextPostSamples;
  if (hasMinValid)
    systemDefaultValue.RcalLoopCount = nextMinValidDeci;
  if (hasCellGain)
    modbusSetCellGain(nextCellGain);
  if (hasCellOffset)
    modbusSetCellOffset(nextCellOffset);
  if (hasUseHoleCt)
    modbusSetUseHoleCt(nextUseHoleCt);
  if (hasAmpOffset)
    modbusSetAmpereOffset(nextAmpOffset);
  if (hasAmpGain)
    modbusSetAmpereGain(nextAmpGain);
  if (hasImpGain)
    systemDefaultValue.impedanceGainPermille = bmsSanitizeImpedanceGainPermille(nextImpGainPermille);
  if (hasImpOffset)
    systemDefaultValue.impedanceOffsetCentiMohm = nextImpOffsetCenti;
  if (hasImpAutoUpdate)
    systemDefaultValue.impedanceAutoUpdateEnabled = nextImpAutoUpdate ? 1u : 0u;
  if (hasRcalReal)
    systemDefaultValue.real_Cal = nextRcalReal;
  if (hasRcalImage)
    systemDefaultValue.image_Cal = nextRcalImage;

  if (hasImpGain || hasImpOffset)
  {
    const float oldGain = (float)oldImpGainPermille / 1000.0f;
    const float oldOffsetMohm = (float)oldImpOffsetCenti / 100.0f;
    const float newGain = (float)bmsSanitizeImpedanceGainPermille(systemDefaultValue.impedanceGainPermille) / 1000.0f;
    const float newOffsetMohm = (float)systemDefaultValue.impedanceOffsetCentiMohm / 100.0f;

    for (int i = 0; i < MAX_INSTALLED_CELLS; i++)
    {
      const float oldBaseMohm = impedanceCompMohmFromCenti(systemDefaultValue.baseImpendance[i]);
      if (oldBaseMohm > 0.0f)
      {
        const float rawBaseMohm = removeGlobalCalibrationFromMohm(oldBaseMohm, oldGain, oldOffsetMohm);
        float newBaseMohm = applyGlobalCalibrationToMohm(rawBaseMohm, newGain, newOffsetMohm);
        int16_t newBaseCenti = impedanceCompCentiFromMohm(newBaseMohm);
        if (newBaseCenti < 0)
          newBaseCenti = 0;
        systemDefaultValue.baseImpendance[i] = newBaseCenti;
      }

      const float compMohm = impedanceCompMohmFromCenti(systemDefaultValue.impendanceCompensation[i]);
      const float currentMohm = cellvalue[i].impendance;
      if (currentMohm > 0.0f)
      {
        const float oldBeforeCellComp = currentMohm - compMohm;
        const float rawCurrentMohm = removeGlobalCalibrationFromMohm(oldBeforeCellComp, oldGain, oldOffsetMohm);
        const float newBeforeCellComp = applyGlobalCalibrationToMohm(rawCurrentMohm, newGain, newOffsetMohm);
        const float newCurrentMohm = newBeforeCellComp + compMohm;
        cellvalue[i].impendance = newCurrentMohm > 0.0f ? newCurrentMohm : 0.0f;
      }
    }
  }

  if (!readnWriteEEProm(true))
  {
    dataSyncUnlockSystemConfig();
    sendJsonError(500, "eeprom commit failed");
    return;
  }
  const uint16_t periodMin = bmsSanitizeImpedancePeriodMin(systemDefaultValue.ImpedanceMeasurePeriod);
  const uint16_t readMax = bmsSanitizeImpedanceReadMax(systemDefaultValue.ACVoltPP);
  const uint16_t stableWindow = bmsSanitizeImpedanceStableWindow(systemDefaultValue.DCVolt, readMax);
  const uint8_t changePercent = bmsSanitizeImpedanceEepromChangePercent(systemDefaultValue.ImpedanceFactor);
  const uint8_t stableTolPercent = bmsSanitizeImpedanceStableTolPercent(systemDefaultValue.VoltageFactor);
  const uint8_t postSamples = bmsSanitizeImpedancePostSamples(systemDefaultValue.TemperatureFactor);
  const uint16_t minValidDeci = bmsSanitizeImpedanceMinValidDeciMohm(systemDefaultValue.RcalLoopCount);
  const uint8_t impAutoUpdateEnabled = systemDefaultValue.impedanceAutoUpdateEnabled ? 1u : 0u;
  const uint16_t impGainPermille = bmsSanitizeImpedanceGainPermille(systemDefaultValue.impedanceGainPermille);
  const int16_t impOffsetCenti = systemDefaultValue.impedanceOffsetCentiMohm;
  const float rcalReal = systemDefaultValue.real_Cal;
  const float rcalImage = systemDefaultValue.image_Cal;
  dataSyncUnlockSystemConfig();

  if (hasRcalReal || hasRcalImage)
    AD5940_UseStoredRcalFromEeprom();

  JsonDocument doc;
  doc["ok"] = true;
  JsonObject bmsControl = doc["bmsControl"].to<JsonObject>();
  bmsControl["cellGain"] = (unsigned)modbusGetCellGain();
  bmsControl["cellOffset"] = (int)modbusGetCellOffset();
  bmsControl["useHoleCT"] = (unsigned)modbusGetUseHoleCt();
  bmsControl["ampereOffset"] = (int)modbusGetAmpereOffset();
  bmsControl["ampereGain"] = (unsigned)modbusGetAmpereGain();
  bmsControl["impedanceEepromChangePercent"] = (unsigned)changePercent;
  bmsControl["impedanceMeasurePeriodMin"] = (unsigned)periodMin;
  bmsControl["impedanceReadMax"] = (unsigned)readMax;
  bmsControl["impedanceStableWindow"] = (unsigned)stableWindow;
  bmsControl["impedanceStableTolPercent"] = (unsigned)stableTolPercent;
  bmsControl["impedancePostStableSamples"] = (unsigned)postSamples;
  bmsControl["impedanceMinValidMohm"] = (float)minValidDeci / 10.0f;
  bmsControl["impedanceAutoUpdateEnabled"] = (unsigned)impAutoUpdateEnabled;
  bmsControl["impedanceGain"] = (float)impGainPermille / 1000.0f;
  bmsControl["impedanceOffsetMohm"] = (float)impOffsetCenti / 100.0f;
  bmsControl["bootRcalReal"] = bootRcalVerifyRealGet();
  bmsControl["bootRcalImage"] = bootRcalVerifyImageGet();
  bmsControl["bootRcalMagnitudeMohm"] = bootRcalVerifyMagnitudeGet();
  bmsControl["rcalReal"] = rcalReal;
  bmsControl["rcalImage"] = rcalImage;
  bmsControl["rcalMagnitudeMohm"] = rcalMagnitudeMohm(rcalReal, rcalImage);
  sendJsonDocumentResponse(200, doc);
}

static void handleApiLogout(void)
{
  sendCorsHeaders();
  sessionClear();
  clearSessionCookie();
  JsonDocument doc;
  doc["ok"] = true;
  sendJsonDocumentResponse(200, doc);
}

static FILE *s_spiffsUploadFp = nullptr;

static const char *contentTypeWithCharset(const char *mime)
{
  if (mime && strcmp(mime, "text/html") == 0)
    return "text/html; charset=utf-8";
  if (mime && strcmp(mime, "text/css") == 0)
    return "text/css; charset=utf-8";
  if (mime && strcmp(mime, "text/javascript") == 0)
    return "text/javascript; charset=utf-8";
  return mime;
}

static void sendFileUploadFallbackPage(void)
{
  s_server.setContentLength(
      strlen_P(WEB_FILE_UPLOAD_STYLE) + strlen_P(WEB_FILE_UPLOAD_BODY));
  s_server.send(200, "text/html; charset=utf-8", "");
  s_server.sendContent_P(WEB_FILE_UPLOAD_STYLE);
  s_server.sendContent_P(WEB_FILE_UPLOAD_BODY);
}

static void *allocIoBuffer(size_t size)
{
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p)
    p = malloc(size);
  return p;
}

static void readFileToWeb(const char *contentType, const char *filename)
{
  struct stat st;
  memset(&st, 0, sizeof(st));
  if (stat(filename, &st) != 0 || st.st_size <= 0)
  {
    String msg = "file not found ";
    msg += filename;
    s_server.send(404, "text/plain", msg);
    ESP_LOGW(TAG, "missing %s", filename);
    return;
  }

  s_server.setContentLength((size_t)st.st_size);
  s_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  s_server.sendHeader("Pragma", "no-cache");
  s_server.sendHeader("Expires", "-1");
  s_server.send(200, contentTypeWithCharset(contentType), "");

  char *buf = (char *)allocIoBuffer(512);
  if (!buf)
  {
    s_server.send(500, "text/plain", "buffer alloc failed");
    return;
  }

  FILE *fp = fopen(filename, "rb");
  if (!fp)
  {
    free(buf);
    s_server.send(404, "text/plain", "open failed");
    return;
  }

  uint32_t chunkCount = 0;
  while (!feof(fp))
  {
    const size_t n = fread(buf, 1, 512, fp);
    if (n > 0)
    {
      s_server.sendContent(buf, n);
      chunkCount++;
      /* 정적 파일 응답 중 AP starvation 방지: 매 chunk마다 양보 */
      delay(1);
    }
  }
  fclose(fp);
  free(buf);
}

static void routeSpiffsFile(const char *uri, const char *mime, const char *spiffsPath)
{
  s_server.on(uri, HTTP_GET, [mime, spiffsPath]()
            { readFileToWeb(mime, spiffsPath); });
}

static void handleJqueryMinJs(void)
{
  sendCorsHeaders();
  s_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  s_server.send_P(200, "application/javascript", jquery_min_js);
}

static void handleFileUploadPage(void)
{
  sendFileUploadFallbackPage();
}

static void handleUploadOptions(void)
{
  sendCorsHeaders();
  s_server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Cookie");
  s_server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  s_server.sendHeader("Connection", "close");
  s_server.send(200, "text/plain", "OK");
}

static String spiffsUploadPath(const String &uploadName)
{
  int slash = uploadName.lastIndexOf('/');
  int backslash = uploadName.lastIndexOf('\\');
  int sep = (slash > backslash) ? slash : backslash;
  String base = (sep >= 0) ? uploadName.substring(sep + 1) : uploadName;
  base.trim();
  if (base.length() == 0 || base.indexOf("..") >= 0)
    return String();
  return String("/spiffs/") + base;
}

static void handleUploadComplete(void)
{
  s_uploadInProgress = false;
  sendCorsHeaders();
  s_server.sendHeader("Connection", "close");
  s_server.send(200, "text/plain", "OK");
}

static void handleUploadBody(void)
{
  HTTPUpload &upload = s_server.upload();

  if (upload.status == UPLOAD_FILE_START)
  {
    s_uploadInProgress = true;
    const String path = spiffsUploadPath(upload.filename);
    if (path.length() == 0)
    {
      ESP_LOGW(TAG, "upload rejected: %s", upload.filename.c_str());
      return;
    }
    s_spiffsUploadFp = fopen(path.c_str(), "wb");
    if (!s_spiffsUploadFp)
      ESP_LOGE(TAG, "fopen failed: %s", path.c_str());
    else
      ESP_LOGI(TAG, "upload start: %s", path.c_str());
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (s_spiffsUploadFp)
      fwrite(upload.buf, 1, upload.currentSize, s_spiffsUploadFp);
    /* 업로드 연속 쓰기 중 AP 응답성 저하 완화 */
    delay(1);
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (s_spiffsUploadFp)
    {
      fclose(s_spiffsUploadFp);
      s_spiffsUploadFp = nullptr;
    }
    ESP_LOGI(TAG, "upload end: %s (%u bytes)", upload.filename.c_str(),
             (unsigned)upload.totalSize);
    s_uploadInProgress = false;
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    if (s_spiffsUploadFp)
    {
      fclose(s_spiffsUploadFp);
      s_spiffsUploadFp = nullptr;
    }
    s_uploadInProgress = false;
    ESP_LOGW(TAG, "upload aborted: %s", upload.filename.c_str());
  }
}

static const char *mimeFromUri(const String &uri)
{
  if (uri.endsWith(".css"))
    return "text/css";
  if (uri.endsWith(".js"))
    return "text/javascript";
  if (uri.endsWith(".map"))
    return "application/json";
  if (uri.endsWith(".html"))
    return "text/html";
  if (uri.endsWith(".ico"))
    return "image/x-icon";
  if (uri.endsWith(".svg"))
    return "image/svg+xml";
  if (uri.endsWith(".png"))
    return "image/png";
  if (uri.endsWith(".gif"))
    return "image/gif";
  if (uri.endsWith(".mp3"))
    return "audio/mpeg";
  return nullptr;
}

static void handleWebNotFound(void)
{
  String uri = s_server.uri();
  const char *mime = mimeFromUri(uri);
  if (!mime)
  {
    String msg = "Not found: ";
    msg += uri;
    s_server.send(404, "text/plain", msg);
    return;
  }
  String path = "/spiffs";
  path += uri;
  readFileToWeb(mime, path.c_str());
}

static uint16_t apiInstalledCells(const nvsSystemSet *cfg)
{
  uint16_t n = cfg ? cfg->installed_cells : 1;
  if (n < 1)
    n = 1;
  if (n > REST_API_HALF_SLOTS)
    n = REST_API_HALF_SLOTS;
  return n;
}

static uint16_t apiVoltageMv(const _cell_value *cells, uint16_t cellIdx)
{
  const float v = cells[cellIdx].voltage;
  if (v <= 0.0f)
    return 0;
  const float mv = v * 1000.0f + 0.5f;
  if (mv > 65535.0f)
    return 65535u;
  return (uint16_t)mv;
}

static uint16_t apiImpedanceReg(const _cell_value *cells, uint16_t cellIdx)
{
  const float z = cells[cellIdx].impendance;
  if (z <= 0.0f)
    return 0;
  const float scaled = z * 100.0f + 0.5f;
  if (scaled > 65535.0f)
    return 65535u;
  return (uint16_t)scaled;
}

static bool apiDeviceHasValidCells(const _cell_value *cells, uint16_t nCells)
{
  for (uint16_t i = 0; i < nCells; i++)
  {
    if (cells[i].voltage >= 0.6f)
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

static void handleApiBattery(void)
{
  nvsSystemSet cfg;
  copySystemConfigSnapshot(&cfg);
  _cell_value snapCells[MAX_INSTALLED_CELLS] = {0};
  dataSyncReadCellSnapshot(snapCells, MAX_INSTALLED_CELLS);

  const uint16_t nCells = apiInstalledCells(&cfg);
  const bool ok = apiDeviceHasValidCells(snapCells, nCells);
  const unsigned devAddr = (unsigned)cfg.modbusId;

  uint16_t samples[REST_API_DATA_SLOTS] = {0};
  for (uint16_t i = 0; i < nCells; i++)
    samples[i] = apiVoltageMv(snapCells, i);
  for (uint16_t i = 0; i < nCells; i++)
    samples[REST_API_HALF_SLOTS + i] = apiImpedanceReg(snapCells, i);

  char ts[32];
  formatIso8601Utc(ts, sizeof(ts));

  const float highV = cfg.alarmHighCellVoltage / 1000.0f;
  const float lowV = cfg.alarmLowCellVoltage / 1000.0f;
  const float tempC = (float)ntcTemperatureC_x10[0] / 10.0f;
  const float currentA = modbusHasCurrentSensor() ? ctCurrentGetCalibratedAmps() : 0.0f;

  JsonDocument doc;
  doc["status"] = "success";
  doc["timestamp"] = ts;

  JsonObject data = doc["data"].to<JsonObject>();
  JsonObject multiData = data["multi_data"].to<JsonObject>();
  JsonObject devices = multiData["devices"].to<JsonObject>();
  JsonObject device = devices[String(devAddr)].to<JsonObject>();
  device["status"] = ok ? "success" : "failed";

  JsonObject deviceData = device["data"].to<JsonObject>();
  JsonArray dataArray = deviceData["data"].to<JsonArray>();
  for (unsigned i = 0; i < REST_API_DATA_SLOTS; i++)
    dataArray.add((unsigned)samples[i]);
  JsonArray baseImpedanceArray = deviceData["baseImpedanceMohm"].to<JsonArray>();
  for (uint16_t i = 0; i < nCells; i++)
    baseImpedanceArray.add(impedanceCompMohmFromCenti(cfg.baseImpendance[i]));
  deviceData["temperature"] = tempC;
  deviceData["current"] = currentA;
  if (!ok)
    device["error"] = "No valid cell data";

  JsonObject summary = multiData["summary"].to<JsonObject>();
  summary["total"] = 1;
  summary["success"] = ok ? 1 : 0;
  summary["failed"] = ok ? 0 : 1;
  if (!ok)
  {
    JsonArray failedDevices = summary["failedDevices"].to<JsonArray>();
    JsonObject failedDevice = failedDevices.add<JsonObject>();
    failedDevice["id"] = devAddr;
    failedDevice["error"] = "No valid cell data";
  }

  JsonObject rackInfo = data["rackInfo"].to<JsonObject>();
  rackInfo["rackno"] = devAddr;
  rackInfo["installedmodule"] = 1;
  rackInfo["totalbatno"] = (unsigned)cfg.installed_cells;
  rackInfo["rackname"] = "아이에프텍(주)";
  rackInfo["installdate"] = "2024-10-28T15:00:00.000Z";
  rackInfo["expiredate"] = "2034-10-27T15:00:00.000Z";
  rackInfo["bat_type"] = "ni-cd";
  rackInfo["nominalvoltage"] = 1.2f;
  rackInfo["highvoltage"] = highV;
  rackInfo["lowvoltage"] = lowV;
  rackInfo["hightemperature"] = (unsigned)cfg.AlarmTemperature;
  rackInfo["highimpedance"] = 10;
  rackInfo["location"] = "주전산실";

  sendCorsHeaders();
  sendJsonDocumentResponse(200, doc);
}

void restApiInit(void)
{
  /** Cookie/Origin 없으면 hasHeader("Cookie")가 항상 false → /api/me 401 */
  const char *headerKeys[] = {"Origin", "Cookie"};
  s_server.collectHeaders(headerKeys, 2);

  s_server.on("/api/health", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/login", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/me", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/logout", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/network-config", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/bms-config", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/rcal-calibration", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/impedance-compensation", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/impedance-baseline/start", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/impedance-baseline/status", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/system-action", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/api/battery", HTTP_OPTIONS, sendApiPreflight);
  s_server.on("/upload", HTTP_OPTIONS, handleUploadOptions);

  s_server.on("/api/health", HTTP_GET, handleApiHealth);
  s_server.on("/api/login", HTTP_POST, handleApiLogin);
  s_server.on("/api/me", HTTP_GET, handleApiMe);
  s_server.on("/api/logout", HTTP_POST, handleApiLogout);
  s_server.on("/api/network-config", HTTP_GET, handleApiNetworkConfigGet);
  s_server.on("/api/network-config", HTTP_POST, handleApiNetworkConfigPost);
  s_server.on("/api/bms-config", HTTP_GET, handleApiBmsConfigGet);
  s_server.on("/api/bms-config", HTTP_POST, handleApiBmsConfigPost);
  s_server.on("/api/rcal-calibration", HTTP_POST, handleApiRcalCalibrationPost);
  s_server.on("/api/impedance-compensation", HTTP_GET, handleApiImpedanceCompensationGet);
  s_server.on("/api/impedance-compensation", HTTP_POST, handleApiImpedanceCompensationPost);
  s_server.on("/api/impedance-baseline/start", HTTP_POST, handleApiImpedanceBaselineStartPost);
  s_server.on("/api/impedance-baseline/status", HTTP_GET, handleApiImpedanceBaselineStatusGet);
  s_server.on("/api/system-action", HTTP_POST, handleApiSystemActionPost);
  s_server.on("/api/battery", HTTP_GET, handleApiBattery);

  s_server.on("/upload", HTTP_POST, handleUploadComplete, handleUploadBody);
  s_server.on("/upload", HTTP_GET, []()
            {
              s_server.send(405, "text/plain", "Method Not Allowed");
            });
  s_server.on("/fileUpload", HTTP_GET, handleFileUploadPage);

  routeSpiffsFile("/", "text/html", "/spiffs/index.html");
  routeSpiffsFile("/index.html", "text/html", "/spiffs/index.html");
  routeSpiffsFile("/login.html", "text/html", "/spiffs/login.html");
  routeSpiffsFile("/basicInfo.html", "text/html", "/spiffs/basicInfo.html");
  routeSpiffsFile("/impTune.html", "text/html", "/spiffs/impTune.html");
  routeSpiffsFile("/snmpTest.html", "text/html", "/spiffs/snmpTest.html");
  routeSpiffsFile("/help.html", "text/html", "/spiffs/help.html");
  routeSpiffsFile("/index.css", "text/css", "/spiffs/index.css");
  routeSpiffsFile("/login.css", "text/css", "/spiffs/login.css");
  routeSpiffsFile("/style.css", "text/css", "/spiffs/style.css");
  s_server.on("/jquery.min.js", HTTP_GET, handleJqueryMinJs);
  routeSpiffsFile("/index.js", "text/javascript", "/spiffs/index.js");
  routeSpiffsFile("/basicInfo.js", "text/javascript", "/spiffs/basicInfo.js");
  routeSpiffsFile("/impTune.js", "text/javascript", "/spiffs/impTune.js");
  routeSpiffsFile("/snmpTest.js", "text/javascript", "/spiffs/snmpTest.js");

  s_server.onNotFound(handleWebNotFound);
  s_server.begin();
  const String ip = WiFi.softAPIP().toString();
  ESP_LOGI(TAG, "Web http://%s  SPIFFS /spiffs  POST /upload  GET /fileUpload", ip.c_str());
}

void restApiHandle(void)
{
  s_server.handleClient();
}

bool restApiIsUploadInProgress(void)
{
  return s_uploadInProgress;
}

#endif
