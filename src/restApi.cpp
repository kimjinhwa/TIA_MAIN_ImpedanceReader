#include "restApi.h"

#ifdef WIFI_AP_MODE

#include <Arduino.h>
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

static bool apiRequireSession(void);
static void jsonAppendEscaped(char *dst, size_t cap, size_t *off, const char *text);

extern _cell_value cellvalue[MAX_INSTALLED_CELLS];
extern nvsSystemSet systemDefaultValue;
extern uint8_t get485Address(void);
extern LittleFileSystem lsFile;

#define SESSION_COOKIE_NAME "session"
#define SESSION_MAX_AGE_SEC (7u * 24u * 60u * 60u)
#define BMS_IMP_PERIOD_DEFAULT_SEC 3600u
#define BMS_EEPROM_CHANGE_DEFAULT_PERCENT 3u
#define BMS_IMP_STABLE_TOL_DEFAULT_PERCENT 3u
#define BMS_IMP_POST_SAMPLES_DEFAULT 5u
#define BMS_IMP_POST_SAMPLES_MAX 20u
#define BMS_IMP_MIN_VALID_DEFAULT_DECI 50u
#define BMS_IMP_READ_MAX_DEFAULT 60u
#define BMS_IMP_READ_MAX_MAX 120u
#define BMS_IMP_STABLE_WINDOW_DEFAULT 5u
#define BMS_IMP_STABLE_WINDOW_MAX 20u
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

static void handleApiHealth(void)
{
  sendCorsHeaders();
  s_server.send(200, "application/json",
                  "{\"ok\":true,\"service\":\"impedance-web\"}");
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

static bool jsonExtractString(const char *body, const char *key, char *out, size_t outLen)
{
  if (!body || !key || !out || outLen < 2)
    return false;

  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(body, pattern);
  if (!p)
    return false;

  p += strlen(pattern);
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != ':')
    return false;
  p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != '"')
    return false;
  p++;

  size_t i = 0;
  while (*p && *p != '"' && i + 1 < outLen)
    out[i++] = *p++;
  out[i] = '\0';
  return i > 0;
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
    s_server.send(405, "application/json", "{\"status\":\"error\",\"message\":\"Method not allowed\"}");
    return;
  }

  const String body = s_server.arg("plain");
  char userid[16] = {0};
  char passwd[16] = {0};
  bool hasUser = jsonExtractString(body.c_str(), "userid", userid, sizeof(userid));
  bool hasPass = jsonExtractString(body.c_str(), "passwd", passwd, sizeof(passwd));

  if (!hasUser && s_server.hasArg("userid"))
    strncpy(userid, s_server.arg("userid").c_str(), sizeof(userid) - 1);
  if (!hasPass && s_server.hasArg("passwd"))
    strncpy(passwd, s_server.arg("passwd").c_str(), sizeof(passwd) - 1);

  trimInPlace(userid);
  trimInPlace(passwd);

  if (userid[0] == '\0' || passwd[0] == '\0')
  {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing credentials\"}");
    return;
  }

  if (!credentialsMatch(userid, passwd))
  {
    ESP_LOGW(TAG, "login failed user='%s'", userid);
    s_server.send(401, "application/json", "{\"ok\":false,\"error\":\"invalid credentials\"}");
    return;
  }

  sessionStart();
  setSessionCookie();
  ESP_LOGI(TAG, "login ok user='%s' tokenLen=%u expMs=%lu",
           userid, (unsigned)g_sessionToken.length(), (unsigned long)s_sessionExpireMs);
  s_server.sendHeader("Connection", "close");
  s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiMe(void)
{
  sendCorsHeaders();

  if (!API_REQUIRE_AUTH)
  {
    s_server.send(200, "application/json", "{\"ok\":true,\"authenticated\":true,\"devAuthBypass\":true}");
    return;
  }

  if (!sessionIsValid())
  {
    const String reqTok = requestSessionCookieValue();
    ESP_LOGW(TAG, "/api/me denied cookieLen=%u srvLen=%u expIn=%ld",
             (unsigned)reqTok.length(), (unsigned)g_sessionToken.length(),
             (long)((int32_t)s_sessionExpireMs - (int32_t)millis()));
    s_server.send(401, "application/json", "{\"ok\":false,\"authenticated\":false}");
    return;
  }

  s_server.send(200, "application/json", "{\"ok\":true,\"authenticated\":true}");
}

static bool apiRequireSession(void)
{
  if (!API_REQUIRE_AUTH)
    return true;
  if (sessionIsValid())
    return true;
  sendCorsHeaders();
  s_server.send(401, "application/json", "{\"ok\":false,\"authenticated\":false}");
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

static void jsonAppendEscaped(char *dst, size_t cap, size_t *off, const char *text)
{
  if (!dst || !off || !text)
    return;
  for (; *text && *off + 2 < cap; text++)
  {
    if (*text == '"' || *text == '\\')
      dst[(*off)++] = '\\';
    dst[(*off)++] = *text;
  }
  dst[*off] = '\0';
}

static const char *jsonFindObjectBody(const char *body, const char *name)
{
  if (!body || !name)
    return nullptr;
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"%s\"", name);
  const char *p = strstr(body, pattern);
  if (!p)
    return nullptr;
  p += strlen(pattern);
  while (*p == ' ' || *p == '\t' || *p == ':')
    p++;
  if (*p != '{')
    return nullptr;
  return p + 1;
}

static bool jsonExtractInObject(const char *objBody, const char *key, char *out, size_t outLen)
{
  if (!objBody || !key || !out || outLen < 2)
    return false;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(objBody, pattern);
  if (!p)
    return false;
  p += strlen(pattern);
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != ':')
    return false;
  p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == '"')
  {
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outLen)
      out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
  }
  if (strncmp(p, "true", 4) == 0)
  {
    strncpy(out, "true", outLen - 1);
    out[outLen - 1] = '\0';
    return true;
  }
  if (strncmp(p, "false", 5) == 0)
  {
    strncpy(out, "false", outLen - 1);
    out[outLen - 1] = '\0';
    return true;
  }
  size_t i = 0;
  while (*p && *p != ',' && *p != '}' && *p != ' ' && i + 1 < outLen)
    out[i++] = *p++;
  out[i] = '\0';
  return i > 0;
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

  char userEsc[24] = {0};
  size_t off = 0;
  jsonAppendEscaped(userEsc, sizeof(userEsc), &off, cfg.userid);

  char ipStr[20];
  char maskStr[20];
  char gwStr[20];
  strncpy(ipStr, ipUint32ToString(cfg.IPADDRESS).c_str(), sizeof(ipStr) - 1);
  strncpy(maskStr, ipUint32ToString(cfg.SUBNETMASK).c_str(), sizeof(maskStr) - 1);
  strncpy(gwStr, ipUint32ToString(cfg.GATEWAY).c_str(), sizeof(gwStr) - 1);

  static char json[768];
  const String macAddress = WiFi.macAddress();
  snprintf(json, sizeof(json),
           "{\"network\":{\"macAddress\":\"%s\",\"ipAddress\":\"%s\",\"subnetMask\":\"%s\",\"gateway\":\"%s\"},"
           "\"snmpAccess\":{\"ipAddress\":\"0.0.0.0\",\"community\":\"public\",\"permission\":\"NOACCESS\"},"
           "\"trapAccess\":{\"ipAddress\":\"0.0.0.0\",\"community\":\"public\",\"accept\":true},"
           "\"webAccess\":{\"webPort\":80,\"accessIp1\":\"0.0.0.0\",\"accessIp2\":\"0.0.0.0\","
           "\"id\":\"%s\",\"pw\":\"\"}}",
           macAddress.c_str(), ipStr, maskStr, gwStr, userEsc);
  s_server.send(200, "application/json", json);
}

static void handleApiNetworkConfigPost(void)
{
  sendCorsHeaders();

  if (!s_server.hasArg("plain"))
  {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
    return;
  }

  const String body = s_server.arg("plain");
  char ipText[20] = {0};
  char subnetText[20] = {0};
  char gatewayText[20] = {0};
  char webId[16] = {0};
  char webPw[16] = {0};

  const char *netObj = jsonFindObjectBody(body.c_str(), "network");
  if (netObj)
  {
    jsonExtractInObject(netObj, "ipAddress", ipText, sizeof(ipText));
    jsonExtractInObject(netObj, "subnetMask", subnetText, sizeof(subnetText));
    jsonExtractInObject(netObj, "gateway", gatewayText, sizeof(gatewayText));
  }

  IPAddress newIp;
  IPAddress newSubnet;
  IPAddress newGateway;
  if (ipText[0] && subnetText[0] && gatewayText[0] &&
      !(newIp.fromString(ipText) && newSubnet.fromString(subnetText) && newGateway.fromString(gatewayText)))
  {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid network ip\"}");
    return;
  }
  dataSyncLockSystemConfig();
  if (ipText[0] && newIp.fromString(ipText))
    systemDefaultValue.IPADDRESS = static_cast<uint32_t>(newIp);
  if (subnetText[0] && newSubnet.fromString(subnetText))
    systemDefaultValue.SUBNETMASK = static_cast<uint32_t>(newSubnet);
  if (gatewayText[0] && newGateway.fromString(gatewayText))
    systemDefaultValue.GATEWAY = static_cast<uint32_t>(newGateway);

  const char *webObj = jsonFindObjectBody(body.c_str(), "webAccess");
  if (webObj)
  {
    jsonExtractInObject(webObj, "id", webId, sizeof(webId));
    jsonExtractInObject(webObj, "pw", webPw, sizeof(webPw));
    trimInPlace(webId);
    trimInPlace(webPw);
    if (webId[0])
      copyCfgField(systemDefaultValue.userid, sizeof(systemDefaultValue.userid), webId);
    if (webPw[0])
      copyCfgField(systemDefaultValue.userpassword, sizeof(systemDefaultValue.userpassword), webPw);
  }

  eepromNvsWriteBlock(&systemDefaultValue);
  const bool saved = EEPROM.commit();
  dataSyncUnlockSystemConfig();

  if (!saved)
  {
    s_server.send(500, "application/json", "{\"ok\":false,\"rebootRequired\":false}");
    return;
  }

  ESP_LOGI(TAG, "network-config saved ip=%s user=%s", ipText, systemDefaultValue.userid);
  s_server.sendHeader("Connection", "close");
  s_server.send(200, "application/json", "{\"ok\":true,\"rebootRequired\":true}");
  delay(800);
  ESP.restart();
}

static uint16_t bmsSanitizeImpedancePeriodSec(uint32_t sec)
{
  if (sec == 0)
    return (uint16_t)BMS_IMP_PERIOD_DEFAULT_SEC;
  if (sec > 65535u)
    return 65535u;
  return (uint16_t)sec;
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

  cfg->ImpedanceMeasurePeriod = (uint16_t)BMS_IMP_PERIOD_DEFAULT_SEC;
  cfg->ImpedanceFactor = (uint8_t)BMS_EEPROM_CHANGE_DEFAULT_PERCENT;
  cfg->ACVoltPP = (uint16_t)BMS_IMP_READ_MAX_DEFAULT;
  cfg->DCVolt = (uint16_t)BMS_IMP_STABLE_WINDOW_DEFAULT;
  cfg->VoltageFactor = (uint8_t)BMS_IMP_STABLE_TOL_DEFAULT_PERCENT;
  cfg->TemperatureFactor = (uint8_t)BMS_IMP_POST_SAMPLES_DEFAULT;
  cfg->RcalLoopCount = (uint16_t)BMS_IMP_MIN_VALID_DEFAULT_DECI;
}

static void handleApiSystemActionPost(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  if (!s_server.hasArg("plain"))
  {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
    return;
  }

  const String body = s_server.arg("plain");
  char action[40] = {0};
  bool hasAction = jsonExtractString(body.c_str(), "action", action, sizeof(action));
  if (!hasAction && s_server.hasArg("action"))
  {
    const String actionArg = s_server.arg("action");
    strncpy(action, actionArg.c_str(), sizeof(action) - 1);
    hasAction = action[0] != '\0';
  }
  if (!hasAction)
  {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing action\"}");
    return;
  }

  if (strcmp(action, "formatFsFast") == 0)
  {
    lsFile.littleFsInitFast(1);
    s_server.send(200, "application/json", "{\"ok\":true,\"action\":\"formatFsFast\",\"done\":true}");
    return;
  }

  if (strcmp(action, "resetSystemDefaults") == 0)
  {
    dataSyncLockSystemConfig();
    applySystemDefaultsLocked(&systemDefaultValue);
    eepromNvsWriteBlock(&systemDefaultValue);
    const bool saved = EEPROM.commit();
    dataSyncUnlockSystemConfig();
    if (!saved)
    {
      s_server.send(500, "application/json", "{\"ok\":false,\"error\":\"eeprom commit failed\"}");
      return;
    }
    s_server.send(200, "application/json", "{\"ok\":true,\"action\":\"resetSystemDefaults\",\"done\":true}");
    return;
  }

  if (strcmp(action, "reboot") == 0)
  {
    s_server.sendHeader("Connection", "close");
    s_server.send(200, "application/json", "{\"ok\":true,\"action\":\"reboot\",\"rebooting\":true}");
    delay(600);
    ESP.restart();
    return;
  }

  s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown action\"}");
}

static void handleApiImpedanceBaselineStartPost(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  if (modbusReg50BaseImpProgress != 0)
  {
    static char busyJson[160];
    snprintf(busyJson, sizeof(busyJson),
             "{\"ok\":false,\"error\":\"baseline scan busy\",\"progressCell\":%u}",
             (unsigned)modbusReg50BaseImpProgress);
    s_server.send(409, "application/json", busyJson);
    return;
  }

  modbusOnFc06Reg50Write(1);
  static char okJson[160];
  snprintf(okJson, sizeof(okJson),
           "{\"ok\":true,\"started\":true,\"progressCell\":%u}",
           (unsigned)modbusReg50BaseImpProgress);
  s_server.send(200, "application/json", okJson);
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

  static char json[256];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"running\":%s,\"progressCell\":%u,\"completedCells\":%u,\"totalCells\":%u,\"percent\":%u}",
           running ? "true" : "false",
           (unsigned)progressCell,
           (unsigned)completed,
           (unsigned)totalCells,
           (unsigned)percent);
  s_server.send(200, "application/json", json);
}

static void handleApiBmsConfigGet(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  nvsSystemSet cfg;
  copySystemConfigSnapshot(&cfg);
  const uint16_t periodSec = bmsSanitizeImpedancePeriodSec(cfg.ImpedanceMeasurePeriod);
  const uint16_t readMax = bmsSanitizeImpedanceReadMax(cfg.ACVoltPP);
  const uint16_t stableWindow = bmsSanitizeImpedanceStableWindow(cfg.DCVolt, readMax);
  const uint8_t changePercent = bmsSanitizeImpedanceEepromChangePercent(cfg.ImpedanceFactor);
  const uint8_t stableTolPercent = bmsSanitizeImpedanceStableTolPercent(cfg.VoltageFactor);
  const uint8_t postSamples = bmsSanitizeImpedancePostSamples(cfg.TemperatureFactor);
  const uint16_t minValidDeci = bmsSanitizeImpedanceMinValidDeciMohm(cfg.RcalLoopCount);

  static char json[512];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"bmsControl\":{"
           "\"impedanceEepromChangePercent\":%u,"
           "\"impedanceMeasurePeriodSec\":%u,"
           "\"impedanceReadMax\":%u,"
           "\"impedanceStableWindow\":%u,"
           "\"impedanceStableTolPercent\":%u,"
           "\"impedancePostStableSamples\":%u,"
           "\"impedanceMinValidMohm\":%.1f"
           "}}",
           (unsigned)changePercent, (unsigned)periodSec,
           (unsigned)readMax, (unsigned)stableWindow,
           (unsigned)stableTolPercent, (unsigned)postSamples,
           (float)minValidDeci / 10.0f);
  s_server.send(200, "application/json", json);
}

static void handleApiBmsConfigPost(void)
{
  if (!apiRequireSession())
    return;
  sendCorsHeaders();

  if (!s_server.hasArg("plain"))
  {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
    return;
  }

  const String body = s_server.arg("plain");
  const char *cfgObj = jsonFindObjectBody(body.c_str(), "bmsControl");
  if (!cfgObj)
    cfgObj = body.c_str();

  char percentText[16] = {0};
  char periodText[16] = {0};
  char readMaxText[16] = {0};
  char stableWindowText[16] = {0};
  char stableTolText[16] = {0};
  char postSamplesText[16] = {0};
  char minValidText[16] = {0};
  const bool hasPercent = jsonExtractInObject(cfgObj, "impedanceEepromChangePercent", percentText, sizeof(percentText));
  const bool hasPeriod = jsonExtractInObject(cfgObj, "impedanceMeasurePeriodSec", periodText, sizeof(periodText));
  const bool hasReadMax = jsonExtractInObject(cfgObj, "impedanceReadMax", readMaxText, sizeof(readMaxText));
  const bool hasStableWindow = jsonExtractInObject(cfgObj, "impedanceStableWindow", stableWindowText, sizeof(stableWindowText));
  const bool hasStableTol = jsonExtractInObject(cfgObj, "impedanceStableTolPercent", stableTolText, sizeof(stableTolText));
  const bool hasPostSamples = jsonExtractInObject(cfgObj, "impedancePostStableSamples", postSamplesText, sizeof(postSamplesText));
  const bool hasMinValid = jsonExtractInObject(cfgObj, "impedanceMinValidMohm", minValidText, sizeof(minValidText));
  uint8_t nextPercent = 0;
  uint16_t nextPeriod = 0;
  uint16_t nextReadMax = 0;
  uint16_t nextStableWindow = 0;
  uint8_t nextStableTol = 0;
  uint8_t nextPostSamples = 0;
  uint16_t nextMinValidDeci = 0;

  if (!hasPercent && !hasPeriod && !hasReadMax && !hasStableWindow && !hasStableTol && !hasPostSamples && !hasMinValid)
  {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"no bmsControl fields\"}");
    return;
  }

  if (hasPercent)
  {
    const long v = strtol(percentText, nullptr, 10);
    if (v < 1 || v > 100)
    {
      s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"impedanceEepromChangePercent out of range(1..100)\"}");
      return;
    }
    nextPercent = (uint8_t)v;
  }

  if (hasPeriod)
  {
    const long v = strtol(periodText, nullptr, 10);
    if (v < 1 || v > 65535)
    {
      s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"impedanceMeasurePeriodSec out of range(1..65535)\"}");
      return;
    }
    nextPeriod = (uint16_t)v;
  }

  if (hasReadMax)
  {
    const long v = strtol(readMaxText, nullptr, 10);
    if (v < 1 || v > (long)BMS_IMP_READ_MAX_MAX)
    {
      s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"impedanceReadMax out of range(1..120)\"}");
      return;
    }
    nextReadMax = (uint16_t)v;
  }

  if (hasStableWindow)
  {
    const long v = strtol(stableWindowText, nullptr, 10);
    if (v < 2 || v > (long)BMS_IMP_STABLE_WINDOW_MAX)
    {
      s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"impedanceStableWindow out of range(2..20)\"}");
      return;
    }
    nextStableWindow = (uint16_t)v;
  }

  if (hasStableTol)
  {
    const long v = strtol(stableTolText, nullptr, 10);
    if (v < 1 || v > 20)
    {
      s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"impedanceStableTolPercent out of range(1..20)\"}");
      return;
    }
    nextStableTol = (uint8_t)v;
  }

  if (hasPostSamples)
  {
    const long v = strtol(postSamplesText, nullptr, 10);
    if (v < 1 || v > (long)BMS_IMP_POST_SAMPLES_MAX)
    {
      s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"impedancePostStableSamples out of range(1..20)\"}");
      return;
    }
    nextPostSamples = (uint8_t)v;
  }

  if (hasMinValid)
  {
    const float v = strtof(minValidText, nullptr);
    if (v < 0.1f || v > 1000.0f)
    {
      s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"impedanceMinValidMohm out of range(0.1..1000.0)\"}");
      return;
    }
    nextMinValidDeci = (uint16_t)(v * 10.0f + 0.5f);
  }

  dataSyncLockSystemConfig();
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
  eepromNvsWriteBlock(&systemDefaultValue);
  if (!EEPROM.commit())
  {
    dataSyncUnlockSystemConfig();
    s_server.send(500, "application/json", "{\"ok\":false,\"error\":\"eeprom commit failed\"}");
    return;
  }
  const uint16_t periodSec = bmsSanitizeImpedancePeriodSec(systemDefaultValue.ImpedanceMeasurePeriod);
  const uint16_t readMax = bmsSanitizeImpedanceReadMax(systemDefaultValue.ACVoltPP);
  const uint16_t stableWindow = bmsSanitizeImpedanceStableWindow(systemDefaultValue.DCVolt, readMax);
  const uint8_t changePercent = bmsSanitizeImpedanceEepromChangePercent(systemDefaultValue.ImpedanceFactor);
  const uint8_t stableTolPercent = bmsSanitizeImpedanceStableTolPercent(systemDefaultValue.VoltageFactor);
  const uint8_t postSamples = bmsSanitizeImpedancePostSamples(systemDefaultValue.TemperatureFactor);
  const uint16_t minValidDeci = bmsSanitizeImpedanceMinValidDeciMohm(systemDefaultValue.RcalLoopCount);
  dataSyncUnlockSystemConfig();

  static char json[512];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"bmsControl\":{"
           "\"impedanceEepromChangePercent\":%u,"
           "\"impedanceMeasurePeriodSec\":%u,"
           "\"impedanceReadMax\":%u,"
           "\"impedanceStableWindow\":%u,"
           "\"impedanceStableTolPercent\":%u,"
           "\"impedancePostStableSamples\":%u,"
           "\"impedanceMinValidMohm\":%.1f"
           "}}",
           (unsigned)changePercent, (unsigned)periodSec,
           (unsigned)readMax, (unsigned)stableWindow,
           (unsigned)stableTolPercent, (unsigned)postSamples,
           (float)minValidDeci / 10.0f);
  s_server.send(200, "application/json", json);
}

static void handleApiLogout(void)
{
  sendCorsHeaders();
  sessionClear();
  clearSessionCookie();
  s_server.send(200, "application/json", "{\"ok\":true}");
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
  s_server.send(204);
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
  sendCorsHeaders();
  s_server.sendHeader("Connection", "close");
  s_server.send(200, "text/plain", "OK");
}

static void handleUploadBody(void)
{
  HTTPUpload &upload = s_server.upload();

  if (upload.status == UPLOAD_FILE_START)
  {
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
  nvsSystemSet cfg;
  copySystemConfigSnapshot(&cfg);
  _cell_value snapCells[MAX_INSTALLED_CELLS] = {0};
  dataSyncReadCellSnapshot(snapCells, MAX_INSTALLED_CELLS);

  const uint16_t nCells = apiInstalledCells(&cfg);
  const bool ok = apiDeviceHasValidCells(snapCells, nCells);
  const unsigned devAddr = (unsigned)get485Address();

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
  const float currentA = (float)packCurrentA_x10 / 10.0f;

  static char json[4096];
  int off = 0;

  off += snprintf(json + off, sizeof(json) - (size_t)off,
                  "{\"status\":\"success\",\"timestamp\":\"%s\",\"data\":{\"multi_data\":{"
                  "\"devices\":{\"%u\":{",
                  ts, devAddr);

  if (ok)
  {
    off += snprintf(json + off, sizeof(json) - (size_t)off, "\"status\":\"success\",\"data\":{");
    off = appendDataArray(json, sizeof(json), off, samples, REST_API_DATA_SLOTS);
    off += snprintf(json + off, sizeof(json) - (size_t)off,
                    ",\"temperature\":%.1f,\"current\":%.1f}}",
                    tempC, currentA);
  }
  else
  {
    off += snprintf(json + off, sizeof(json) - (size_t)off,
                    "\"status\":\"failed\",\"data\":{");
    off = appendDataArray(json, sizeof(json), off, samples, REST_API_DATA_SLOTS);
    off += snprintf(json + off, sizeof(json) - (size_t)off,
                    ",\"temperature\":%.1f,\"current\":%.1f},\"error\":\"No valid cell data\"}",
                    tempC, currentA);
  }

  off += snprintf(json + off, sizeof(json) - (size_t)off,
                  "},\"summary\":{\"total\":1,\"success\":%u,\"failed\":%u",
                  ok ? 1u : 0u, ok ? 0u : 1u);

  if (!ok)
  {
    off += snprintf(json + off, sizeof(json) - (size_t)off,
                    ",\"failedDevices\":[{\"id\":%u,\"error\":\"No valid cell data\"}]",
                    devAddr);
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
                  devAddr,
                  (unsigned)cfg.installed_cells,
                  highV, lowV,
                  (unsigned)cfg.AlarmTemperature);

  sendCorsHeaders();
  s_server.send(200, "application/json", json);
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
  routeSpiffsFile("/snmpTest.html", "text/html", "/spiffs/snmpTest.html");
  routeSpiffsFile("/help.html", "text/html", "/spiffs/help.html");
  routeSpiffsFile("/index.css", "text/css", "/spiffs/index.css");
  routeSpiffsFile("/login.css", "text/css", "/spiffs/login.css");
  routeSpiffsFile("/style.css", "text/css", "/spiffs/style.css");
  s_server.on("/jquery.min.js", HTTP_GET, handleJqueryMinJs);
  routeSpiffsFile("/index.js", "text/javascript", "/spiffs/index.js");
  routeSpiffsFile("/basicInfo.js", "text/javascript", "/spiffs/basicInfo.js");
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

#endif
