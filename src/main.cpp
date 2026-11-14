#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <SPI.h>
#include <WiFi.h>
#include <bb_epaper.h>
#include <Fonts/Roboto_Black_16.h>
#include <Fonts/Roboto_Black_10.h>
#include <Fonts/Roboto_Black_24.h>
#include <Fonts/Roboto_Black_32.h>
#include <Fonts/Roboto_Black_40.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include <cctype>
#include <cstring>
#include <new>

#include "app_config.h"
#include "connectivity.h"
#include "web_assets.h"

namespace {

enum class DisplayState : uint8_t { Ready, Queued, Updating, Error };

struct UploadContext {
  uint8_t *buffer = nullptr;
  size_t received = 0;
  size_t expected = 0;
  int errorCode = 0;
  char errorMessage[96] = {0};
};

struct ProjectUploadContext {
  File file;
  size_t received = 0;
  size_t expected = 0;
  int errorCode = 0;
  char errorMessage[96] = {0};
};

struct StatusSnapshot {
  DisplayState state;
  uint32_t job;
  char message[96];
};

struct ButtonState {
  explicit ButtonState(uint8_t buttonPin) : pin(buttonPin) {}

  uint8_t pin;
  bool lastReading = HIGH;
  bool stableState = HIGH;
  uint32_t changedAt = 0;
};

struct LiveClockRegion {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
  bool showTime = true;
  bool showWeekday = true;
  bool showDate = true;
  bool longDate = false;
  bool inverted = false;
  bool border = false;
  bool twelveHour = false;
};

AsyncWebServer server(80);
DNSServer dnsServer;
BBEPAPER display;

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
DisplayState displayState = DisplayState::Ready;
uint32_t currentJob = 0;
char stateMessage[96] = "Preparado";
uint8_t *pendingBmp = nullptr;
bool pendingDynamic = false;
uint8_t pendingDynamicLayers = 0;
int8_t pendingSolid = -1;
bool displayReady = false;
ButtonState key1{AppConfig::KEY1};
ButtonState key2{AppConfig::KEY2};
ButtonState key3{AppConfig::KEY3};
volatile uint32_t busyTransitions = 0;
uint32_t lastDiagnosticAt = 0;
bool storageReady = false;
bool projectSaving = false;
constexpr size_t MAX_LIVE_CLOCKS = 6;
LiveClockRegion liveClocks[MAX_LIVE_CLOCKS];
size_t liveClockCount = 0;
bool refreshEveryMinute = false;
bool refreshEveryHour = false;
bool refreshAtMidnight = false;
time_t nextDynamicRefreshAt = 0;
char nextDynamicReason[48] = "";
char activePageId[80] = "";
char activeFrameId[65] = "";
bool globalInvert = false;
bool runtimeInvert = false;
bool runtimeBlank = false;
int displayRotation = 0;
time_t lastRefreshAt = 0;
uint32_t lastLiveRefreshCheck = 0;
time_t lastBackgroundWeatherHour = 0;
int lastBackgroundHolidayDay = -1;

constexpr char PROJECT_FILE[] = "/project.json";
constexpr char PROJECT_TEMP_FILE[] = "/project.upload";
constexpr char PROJECT_BACKUP_FILE[] = "/project.backup";
constexpr char DISPLAY_FILE[] = "/display.bmp";
constexpr char DISPLAY_TEMP_FILE[] = "/display.upload";
constexpr char RUNTIME_FILE[] = "/runtime.json";
constexpr size_t STORAGE_RESERVE = 256 * 1024;

void loadLiveConfiguration();
void destroyUploadContext(UploadContext *context);
void setUploadError(UploadContext *context, int code, const char *message);
bool validHash(const String &value);
String resourcePath(const char *folder, const String &id);
String sha256Hex(const uint8_t *data, size_t length);
enum DynamicLayer : uint8_t {
  DYNAMIC_CLOCK = 1 << 0,
  DYNAMIC_WEATHER = 1 << 1,
  DYNAMIC_AGENDA = 1 << 2,
  DYNAMIC_ALL = DYNAMIC_CLOCK | DYNAMIC_WEATHER | DYNAMIC_AGENDA,
};

bool queueDynamicRefresh(const char *message, uint8_t layers = DYNAMIC_CLOCK);
void persistRuntimeState();

void IRAM_ATTR handleBusyChange() {
  ++busyTransitions;
}

uint16_t readU16(const uint8_t *data, size_t offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8U);
}

uint32_t readU32(const uint8_t *data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8U) |
         (static_cast<uint32_t>(data[offset + 2]) << 16U) | (static_cast<uint32_t>(data[offset + 3]) << 24U);
}

const char *stateName(DisplayState state) {
  switch (state) {
    case DisplayState::Ready:
      return "ready";
    case DisplayState::Queued:
      return "queued";
    case DisplayState::Updating:
      return "updating";
    case DisplayState::Error:
      return "error";
  }
  return "error";
}

void setState(DisplayState state, const char *message) {
  portENTER_CRITICAL(&stateMux);
  displayState = state;
  strlcpy(stateMessage, message, sizeof(stateMessage));
  portEXIT_CRITICAL(&stateMux);
}

StatusSnapshot getStatusSnapshot() {
  StatusSnapshot snapshot{};
  portENTER_CRITICAL(&stateMux);
  snapshot.state = displayState;
  snapshot.job = currentJob;
  strlcpy(snapshot.message, stateMessage, sizeof(snapshot.message));
  portEXIT_CRITICAL(&stateMux);
  return snapshot;
}

bool isBusy() {
  portENTER_CRITICAL(&stateMux);
  const bool busy = displayState == DisplayState::Queued || displayState == DisplayState::Updating ||
                    pendingBmp != nullptr || pendingDynamic || pendingSolid >= 0;
  portEXIT_CRITICAL(&stateMux);
  return busy;
}

void sendJsonMessage(AsyncWebServerRequest *request, int code, const char *message, uint32_t job = 0) {
  char payload[192];
  snprintf(payload, sizeof(payload), "{\"message\":\"%s\",\"job\":%lu}", message,
           static_cast<unsigned long>(job));
  AsyncWebServerResponse *response = request->beginResponse(code, "application/json; charset=utf-8", payload);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void serveCompressed(AsyncWebServerRequest *request, const char *contentType, const uint8_t *data, size_t length) {
  AsyncWebServerResponse *response = request->beginResponse(200, contentType, data, length);
  response->addHeader("Content-Encoding", "gzip");
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("X-Content-Type-Options", "nosniff");
  request->send(response);
}

void serveIndex(AsyncWebServerRequest *request) {
  serveCompressed(request, "text/html; charset=utf-8", WebAssets::INDEX_HTML_GZ, WebAssets::INDEX_HTML_GZ_LEN);
}

void handleStatus(AsyncWebServerRequest *request) {
  const StatusSnapshot snapshot = getStatusSnapshot();
  char payload[320];
  snprintf(payload, sizeof(payload),
           "{\"state\":\"%s\",\"message\":\"%s\",\"job\":%lu,\"width\":%u,\"height\":%u,"
           "\"ip\":\"192.168.4.1\"}",
           stateName(snapshot.state), snapshot.message, static_cast<unsigned long>(snapshot.job), AppConfig::DISPLAY_WIDTH,
           AppConfig::DISPLAY_HEIGHT);
  AsyncWebServerResponse *response = request->beginResponse(200, "application/json; charset=utf-8", payload);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

// Datos disponibles en esta primera prueba. Los sensores que el kit no
// expone quedan a null para que el widget los trate como no disponibles.
void handleDeviceStatus(AsyncWebServerRequest *request) {
  const StatusSnapshot snapshot = getStatusSnapshot();
  JsonDocument document;
  document["source"] = "hardware";
  document["updatedAt"] = millis();
  document["connected"] = true;
  document["error"] = nullptr;
  document["power"]["percentage"] = nullptr;
  document["power"]["charging"] = nullptr;
  document["power"]["voltage"] = nullptr;
  document["network"]["state"] = Connectivity::connected() ? "connected" : "ap";
  document["network"]["ssid"] = Connectivity::ssid();
  document["network"]["ip"] = Connectivity::ip();
  if (Connectivity::connected()) document["network"]["rssi"] = Connectivity::rssi();
  else document["network"]["rssi"] = nullptr;
  document["network"]["internet"] = Connectivity::connected();
  document["network"]["weatherConfigured"] = Connectivity::weatherConfigured();
  document["display"]["state"] = stateName(snapshot.state);
  if (lastRefreshAt > 0) {
    struct tm utc {};
    gmtime_r(&lastRefreshAt, &utc);
    char timestamp[24];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    document["display"]["lastRefreshAt"] = timestamp;
  } else document["display"]["lastRefreshAt"] = nullptr;
  document["display"]["refreshCount"] = snapshot.job;
  document["display"]["refreshMode"] = "full";
  document["display"]["liveClockCount"] = liveClockCount;
  document["display"]["activePageId"] = activePageId;
  document["display"]["nextRefreshReason"] = nextDynamicRefreshAt ? nextDynamicReason : nullptr;
  if (nextDynamicRefreshAt > 0) {
    struct tm utc {};
    gmtime_r(&nextDynamicRefreshAt, &utc);
    char timestamp[24];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    document["display"]["nextRefreshAt"] = timestamp;
  } else document["display"]["nextRefreshAt"] = nullptr;
  document["storage"]["total"] = LittleFS.totalBytes();
  document["storage"]["used"] = LittleFS.usedBytes();
  document["storage"]["free"] = LittleFS.totalBytes() - LittleFS.usedBytes();
  document["storage"]["reserved"] = STORAGE_RESERVE;
  document["system"]["uptimeSeconds"] = millis() / 1000UL;
  document["system"]["uptimeLabel"] = String(millis() / 60000UL) + " min";
  document["system"]["firmware"] = "MiEInk Studio 1.0";
  document["system"]["temperatureC"] = nullptr;
  String payload;
  serializeJson(document, payload);
  AsyncWebServerResponse *response = request->beginResponse(200, "application/json; charset=utf-8", payload);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void destroyProjectUploadContext(ProjectUploadContext *context) {
  if (context == nullptr) return;
  if (context->file) context->file.close();
  delete context;
}

void setProjectUploadError(ProjectUploadContext *context, int code, const char *message) {
  if (context == nullptr || context->errorCode != 0) return;
  context->errorCode = code;
  strlcpy(context->errorMessage, message, sizeof(context->errorMessage));
  Serial.printf("[PROJECT][ERROR] %d: %s\n", code, message);
}

bool projectLooksSafeJson() {
  File file = LittleFS.open(PROJECT_TEMP_FILE, "r");
  if (!file) return false;
  bool started = false;
  String recent;
  while (file.available()) {
    const int value = file.read();
    if (!started) {
      if (value == ' ' || value == '\n' || value == '\r' || value == '\t') continue;
      if (value != '{') {
        file.close();
        return false;
      }
      started = true;
    }
    char character = static_cast<char>(value);
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    recent += character;
    if (recent.length() > 20) recent.remove(0, recent.length() - 20);
    // Las contraseñas nunca forman parte de un proyecto MiEInk.
    if (recent.indexOf("password") >= 0 || recent.indexOf("passphrase") >= 0) {
      file.close();
      return false;
    }
  }
  file.close();
  return started;
}

bool projectReferencesValid(JsonDocument &document) {
  File file = LittleFS.open(PROJECT_TEMP_FILE, "r");
  if (!file) return false;
  const DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error || (document["schemaVersion"] | 0) != 7 || !document["pages"].is<JsonArray>()) return false;
  for (JsonObjectConst page : document["pages"].as<JsonArrayConst>()) {
    const String frameId = page["frameId"] | "";
    if (!validHash(frameId) || !LittleFS.exists(resourcePath("frames", frameId))) return false;
  }
  for (JsonPairConst asset : document["assets"].as<JsonObjectConst>()) {
    const String id = asset.key().c_str();
    if (!validHash(id) || !LittleFS.exists(resourcePath("assets", id))) return false;
  }
  return true;
}

void cleanupResourceFolder(const char *folder, JsonDocument &document, bool frames) {
  File directory = LittleFS.open(String("/") + folder);
  if (!directory || !directory.isDirectory()) return;
  File entry = directory.openNextFile();
  while (entry) {
    const String path = entry.path();
    entry.close();
    const int slash = path.lastIndexOf('/');
    const int dot = path.lastIndexOf('.');
    const String id = path.substring(slash + 1, dot > slash ? dot : path.length());
    bool referenced = false;
    if (frames) {
      for (JsonObjectConst page : document["pages"].as<JsonArrayConst>()) {
        if (String(page["frameId"] | "") == id) { referenced = true; break; }
      }
    } else referenced = !document["assets"][id].isNull();
    if (!referenced && !path.endsWith(".upload")) LittleFS.remove(path);
    if (path.endsWith(".upload")) LittleFS.remove(path);
    entry = directory.openNextFile();
  }
  directory.close();
}

void cleanupUnreferencedResources(JsonDocument &document) {
  cleanupResourceFolder("assets", document, false);
  cleanupResourceFolder("frames", document, true);
  File directory = LittleFS.open("/weather");
  if (!directory || !directory.isDirectory()) return;
  File entry = directory.openNextFile();
  while (entry) {
    const String path = entry.path();
    entry.close();
    bool referenced = false;
    for (JsonObjectConst page : document["pages"].as<JsonArrayConst>()) {
      for (JsonObjectConst widget : page["widgets"].as<JsonArrayConst>()) {
        if (String(widget["type"] | "") != "weather") continue;
        const String widgetId = widget["id"] | "";
        const String hash = sha256Hex(reinterpret_cast<const uint8_t *>(widgetId.c_str()), widgetId.length());
        if (path.indexOf(hash.substring(0, 24)) >= 0) { referenced = true; break; }
      }
      if (referenced) break;
    }
    if (!referenced) LittleFS.remove(path);
    entry = directory.openNextFile();
  }
  directory.close();
}

time_t nextLocalBoundary(time_t now, bool hour, bool day) {
  struct tm local {};
  localtime_r(&now, &local);
  local.tm_sec = 0;
  if (day) {
    local.tm_min = 0;
    local.tm_hour = 0;
    local.tm_mday += 1;
  } else if (hour) {
    local.tm_min = 0;
    local.tm_hour += 1;
  } else {
    local.tm_min += 1;
  }
  return mktime(&local);
}

void calculateNextDynamicRefresh(time_t now) {
  nextDynamicRefreshAt = 0;
  nextDynamicReason[0] = '\0';
  if (now < 1700000000) return;
  auto add = [&](time_t candidate, const char *reason) {
    if (nextDynamicRefreshAt == 0 || candidate < nextDynamicRefreshAt) {
      nextDynamicRefreshAt = candidate;
      strlcpy(nextDynamicReason, reason, sizeof(nextDynamicReason));
    } else if (candidate == nextDynamicRefreshAt && strstr(nextDynamicReason, reason) == nullptr) {
      strlcat(nextDynamicReason, ", ", sizeof(nextDynamicReason));
      strlcat(nextDynamicReason, reason, sizeof(nextDynamicReason));
    }
  };
  if (refreshEveryMinute) add(nextLocalBoundary(now, false, false), "hora");
  if (refreshEveryHour) add(nextLocalBoundary(now, true, false), "tiempo");
  if (refreshAtMidnight) add(nextLocalBoundary(now, false, true), "fecha/calendario");
}

void loadLiveConfiguration() {
  liveClockCount = 0;
  refreshEveryMinute = false;
  refreshEveryHour = false;
  refreshAtMidnight = false;
  activeFrameId[0] = '\0';
  if (!storageReady || !LittleFS.exists(PROJECT_FILE)) return;

  String runtimePage;
  if (LittleFS.exists(RUNTIME_FILE)) {
    File runtimeFile = LittleFS.open(RUNTIME_FILE, "r");
    JsonDocument runtime;
    if (runtimeFile && !deserializeJson(runtime, runtimeFile)) {
      runtimePage = runtime["activePageId"] | "";
      runtimeInvert = runtime["inverted"] | false;
      runtimeBlank = runtime["blank"] | false;
    }
    if (runtimeFile) runtimeFile.close();
  }

  File file = LittleFS.open(PROJECT_FILE, "r");
  if (!file) return;
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error) {
    Serial.printf("[LIVE][WARN] No se pudo leer la configuración: %s\n", error.c_str());
    return;
  }

  displayRotation = document["device"]["display"]["rotation"] | 0;
  globalInvert = document["device"]["display"]["invert"] | false;
  String selectedPage = runtimePage;
  bool runtimePageExists = false;
  for (JsonObjectConst page : document["pages"].as<JsonArrayConst>()) {
    if (String(page["id"] | "") == runtimePage) runtimePageExists = true;
  }
  if (!runtimePageExists) selectedPage = document["activePageId"] | "";
  strlcpy(activePageId, selectedPage.c_str(), sizeof(activePageId));

  for (JsonObjectConst page : document["pages"].as<JsonArrayConst>()) {
    if (String(page["id"] | "") != selectedPage) continue;
    strlcpy(activeFrameId, String(page["frameId"] | "").c_str(), sizeof(activeFrameId));
    for (JsonObjectConst widget : page["widgets"].as<JsonArrayConst>()) {
      const String type = widget["type"] | "";
      JsonObjectConst config = widget["config"];
      if (type == "weather") refreshEveryHour = true;
      if (type == "calendar" || type == "events") refreshAtMidnight = true;
      if (type != "clock") continue;
      const bool showTime = config["showTime"] | true;
      const bool showDate = config["showDate"] | true;
      const bool showWeekday = config["showWeekday"] | true;
      refreshEveryMinute = refreshEveryMinute || showTime;
      refreshAtMidnight = refreshAtMidnight || (!showTime && (showDate || showWeekday));
      if (liveClockCount >= MAX_LIVE_CLOCKS) continue;
      LiveClockRegion &clock = liveClocks[liveClockCount++];
      clock.x = constrain(widget["x"].as<int>() * 40, 0, 799);
      clock.y = constrain(widget["y"].as<int>() * 40, 0, 479);
      clock.width = constrain(widget["w"].as<int>() * 40, 1, 800 - clock.x);
      clock.height = constrain(widget["h"].as<int>() * 40, 1, 480 - clock.y);
      clock.showTime = showTime;
      clock.showWeekday = showWeekday;
      clock.showDate = showDate;
      clock.longDate = String(config["dateStyle"] | "short") == "long";
      clock.inverted = config["invert"] | false;
      clock.border = config["border"] | false;
      clock.twelveHour = String(config["format"] | "24") == "12";
    }
    break;
  }
  calculateNextDynamicRefresh(time(nullptr));
  Serial.printf("[LIVE] pagina=%s relojes=%u minuto=%s hora=%s medianoche=%s\n", activePageId,
                static_cast<unsigned>(liveClockCount), refreshEveryMinute ? "SI" : "NO",
                refreshEveryHour ? "SI" : "NO", refreshAtMidnight ? "SI" : "NO");
}

void handleProjectGet(AsyncWebServerRequest *request) {
  if (!storageReady) {
    sendJsonMessage(request, 503, "El almacenamiento del proyecto no está disponible");
    return;
  }
  if (!LittleFS.exists(PROJECT_FILE)) {
    sendJsonMessage(request, 404, "Aún no hay un proyecto guardado");
    return;
  }
  AsyncWebServerResponse *response = request->beginResponse(LittleFS, PROJECT_FILE, "application/json; charset=utf-8", false);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void handleProjectBody(AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t index, size_t total) {
  auto *context = static_cast<ProjectUploadContext *>(request->_tempObject);
  if (index == 0) {
    context = new (std::nothrow) ProjectUploadContext();
    request->_tempObject = context;
    if (context == nullptr) return;

    request->onDisconnect([request]() {
      auto *orphan = static_cast<ProjectUploadContext *>(request->_tempObject);
      request->_tempObject = nullptr;
      if (orphan != nullptr) {
        if (orphan->file) orphan->file.close();
        LittleFS.remove(PROJECT_TEMP_FILE);
        projectSaving = false;
        delete orphan;
      }
    });

    if (!storageReady) {
      setProjectUploadError(context, 503, "El almacenamiento del proyecto no está disponible");
    } else if (projectSaving) {
      setProjectUploadError(context, 409, "Ya se está guardando otro proyecto");
    } else if (!request->contentType().startsWith("application/json")) {
      setProjectUploadError(context, 415, "Content-Type debe ser application/json");
    } else if (total == 0 || total > AppConfig::PROJECT_MAX_SIZE) {
      setProjectUploadError(context, 413, "El proyecto supera el tamaño máximo permitido");
    } else if (LittleFS.usedBytes() + total + STORAGE_RESERVE > LittleFS.totalBytes()) {
      setProjectUploadError(context, 507, "No hay espacio para guardar el proyecto de forma atómica");
    } else {
      LittleFS.remove(PROJECT_TEMP_FILE);
      context->file = LittleFS.open(PROJECT_TEMP_FILE, "w");
      context->expected = total;
      if (!context->file) setProjectUploadError(context, 500, "No se pudo abrir el almacenamiento del proyecto");
      else projectSaving = true;
    }
  }

  context = static_cast<ProjectUploadContext *>(request->_tempObject);
  if (context == nullptr || context->errorCode != 0 || !context->file) return;
  if (index + length > context->expected || context->file.write(data, length) != length) {
    setProjectUploadError(context, 500, "No se pudo escribir el proyecto completo");
    return;
  }
  context->received = index + length;
}

void finishProjectUpload(AsyncWebServerRequest *request) {
  auto *context = static_cast<ProjectUploadContext *>(request->_tempObject);
  request->_tempObject = nullptr;
  if (context == nullptr) {
    sendJsonMessage(request, 500, "No se pudo preparar el guardado del proyecto");
    return;
  }

  if (context->file) context->file.close();
  JsonDocument validatedProject;
  if (context->errorCode != 0 || context->received != context->expected || !projectLooksSafeJson() ||
      !projectReferencesValid(validatedProject)) {
    const int code = context->errorCode ? context->errorCode : 400;
    const char *message = context->errorCode ? context->errorMessage : "Proyecto JSON inválido o contiene datos sensibles";
    LittleFS.remove(PROJECT_TEMP_FILE);
    projectSaving = false;
    destroyProjectUploadContext(context);
    sendJsonMessage(request, code, message);
    return;
  }

  const bool hadProject = LittleFS.exists(PROJECT_FILE);
  LittleFS.remove(PROJECT_BACKUP_FILE);
  if (hadProject && !LittleFS.rename(PROJECT_FILE, PROJECT_BACKUP_FILE)) {
    LittleFS.remove(PROJECT_TEMP_FILE);
    projectSaving = false;
    destroyProjectUploadContext(context);
    sendJsonMessage(request, 500, "No se pudo preparar la sustitución del proyecto");
    return;
  }
  if (!LittleFS.rename(PROJECT_TEMP_FILE, PROJECT_FILE)) {
    if (hadProject) LittleFS.rename(PROJECT_BACKUP_FILE, PROJECT_FILE);
    projectSaving = false;
    destroyProjectUploadContext(context);
    sendJsonMessage(request, 500, "No se pudo guardar el proyecto");
    return;
  }
  LittleFS.remove(PROJECT_BACKUP_FILE);
  projectSaving = false;
  const size_t savedSize = context->received;
  destroyProjectUploadContext(context);
  Serial.printf("[PROJECT] Guardado persistente: %u bytes\n", static_cast<unsigned>(savedSize));
  loadLiveConfiguration();
  cleanupUnreferencedResources(validatedProject);
  queueDynamicRefresh("Configuración sincronizada");
  sendJsonMessage(request, 200, "Proyecto guardado en el controlador");
}

bool validateBmp(const uint8_t *bmp, size_t length) {
  if (bmp == nullptr || length != AppConfig::BMP_FILE_SIZE) return false;
  if (bmp[0] != 'B' || bmp[1] != 'M') return false;
  if (readU32(bmp, 2) != AppConfig::BMP_FILE_SIZE) return false;
  if (readU32(bmp, 10) != AppConfig::BMP_HEADER_SIZE) return false;
  if (readU32(bmp, 14) != 40) return false;
  if (readU32(bmp, 18) != AppConfig::DISPLAY_WIDTH || readU32(bmp, 22) != AppConfig::DISPLAY_HEIGHT) return false;
  if (readU16(bmp, 26) != 1 || readU16(bmp, 28) != 1) return false;
  if (readU32(bmp, 30) != 0 || readU32(bmp, 34) != AppConfig::BMP_PIXEL_SIZE) return false;

  // El generador usa índice 0 negro e índice 1 blanco.
  const bool blackPalette = bmp[54] == 0 && bmp[55] == 0 && bmp[56] == 0;
  const bool whitePalette = bmp[58] == 255 && bmp[59] == 255 && bmp[60] == 255;
  return blackPalette && whitePalette;
}

bool validateAssetBmp(const uint8_t *bmp, size_t length, uint16_t &width, uint16_t &height) {
  if (bmp == nullptr || length < 66 || bmp[0] != 'B' || bmp[1] != 'M') return false;
  const uint32_t parsedWidth = readU32(bmp, 18);
  const uint32_t parsedHeight = readU32(bmp, 22);
  if (parsedWidth == 0 || parsedHeight == 0 || parsedWidth > AppConfig::DISPLAY_WIDTH || parsedHeight > AppConfig::DISPLAY_HEIGHT) return false;
  const uint32_t stride = ((parsedWidth + 31U) / 32U) * 4U;
  const uint32_t pixels = stride * parsedHeight;
  if (readU32(bmp, 2) != length || readU32(bmp, 10) != 62 || readU32(bmp, 14) != 40) return false;
  if (readU16(bmp, 26) != 1 || readU16(bmp, 28) != 1 || readU32(bmp, 30) != 0 || readU32(bmp, 34) != pixels) return false;
  if (length != 62U + pixels) return false;
  const bool palette = bmp[54] == 0 && bmp[55] == 0 && bmp[56] == 0 && bmp[58] == 255 && bmp[59] == 255 && bmp[60] == 255;
  width = static_cast<uint16_t>(parsedWidth);
  height = static_cast<uint16_t>(parsedHeight);
  return palette;
}

String sha256Hex(const uint8_t *data, size_t length) {
  uint8_t digest[32];
  mbedtls_sha256(data, length, digest, 0);
  char hex[65];
  for (size_t index = 0; index < sizeof(digest); ++index) snprintf(hex + index * 2, 3, "%02x", digest[index]);
  hex[64] = '\0';
  return String(hex);
}

uint32_t crc32Of(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc ^ 0xffffffffU;
}

bool validHash(const String &value) {
  if (value.length() != 64) return false;
  for (size_t index = 0; index < value.length(); ++index) {
    if (!isxdigit(static_cast<unsigned char>(value[index]))) return false;
  }
  return true;
}

String resourcePath(const char *folder, const String &id) {
  return String("/") + folder + "/" + id + ".bmp";
}

String resourceIdFromRequest(AsyncWebServerRequest *request, const char *prefix) {
  const String url = request->url();
  if (!url.startsWith(prefix)) return String();
  const String id = url.substring(strlen(prefix));
  return validHash(id) ? id : String();
}

bool hasStorageFor(size_t bytes, const String &replacing = "") {
  size_t replaced = 0;
  if (!replacing.isEmpty() && LittleFS.exists(replacing)) {
    File prior = LittleFS.open(replacing, "r");
    if (prior) replaced = prior.size();
    if (prior) prior.close();
  }
  const size_t used = LittleFS.usedBytes();
  const size_t total = LittleFS.totalBytes();
  return used - min(used, replaced) + bytes + STORAGE_RESERVE <= total;
}

void handleResourceBody(AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t index, size_t total) {
  auto *context = static_cast<UploadContext *>(request->_tempObject);
  if (index == 0) {
    context = new (std::nothrow) UploadContext();
    request->_tempObject = context;
    if (context == nullptr) return;
    context->expected = total;
    if (!storageReady) setUploadError(context, 503, "LittleFS no está disponible");
    else if (!request->contentType().startsWith("image/bmp")) setUploadError(context, 415, "Se requiere image/bmp monocromo");
    else if (total < 66 || total > AppConfig::BMP_FILE_SIZE) setUploadError(context, 400, "Tamaño de BMP monocromo no válido");
    else {
      context->buffer = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (context->buffer == nullptr) context->buffer = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_8BIT));
      if (context->buffer == nullptr) setUploadError(context, 500, "No hay memoria para recibir el recurso");
    }
  }
  context = static_cast<UploadContext *>(request->_tempObject);
  if (context == nullptr || context->errorCode || context->buffer == nullptr) return;
  if (index + length > context->expected) return setUploadError(context, 400, "Cuerpo de recurso demasiado grande");
  memcpy(context->buffer + index, data, length);
  context->received = index + length;
}

void finishResourceUpload(AsyncWebServerRequest *request, bool frame) {
  auto *context = static_cast<UploadContext *>(request->_tempObject);
  request->_tempObject = nullptr;
  if (context == nullptr) return sendJsonMessage(request, 500, "No se pudo preparar la carga");
  if (context->errorCode) {
    sendJsonMessage(request, context->errorCode, context->errorMessage);
    return destroyUploadContext(context);
  }
  const String id = resourceIdFromRequest(request, frame ? "/api/frames/" : "/api/assets/");
  uint16_t width = 0, height = 0;
  const bool valid = validHash(id) && context->received == context->expected &&
                     validateAssetBmp(context->buffer, context->received, width, height) &&
                     sha256Hex(context->buffer, context->received).equalsIgnoreCase(id) &&
                     (!frame || (width == AppConfig::DISPLAY_WIDTH && height == AppConfig::DISPLAY_HEIGHT));
  if (!valid) {
    sendJsonMessage(request, 400, "BMP 1-bit inválido o SHA-256 incorrecto");
    return destroyUploadContext(context);
  }
  const String path = resourcePath(frame ? "frames" : "assets", id);
  const bool alreadyStored = LittleFS.exists(path);
  if (!alreadyStored && !hasStorageFor(context->received)) {
    sendJsonMessage(request, 507, "No hay espacio conservando la reserva de 256 KB");
    return destroyUploadContext(context);
  }
  if (!alreadyStored) {
    const String temporary = path + ".upload";
    LittleFS.remove(temporary);
    File file = LittleFS.open(temporary, "w");
    const bool written = file && file.write(context->buffer, context->received) == context->received;
    if (file) { file.flush(); file.close(); }
    if (!written) {
      LittleFS.remove(temporary);
      sendJsonMessage(request, 500, "No se pudo guardar el recurso completo");
      return destroyUploadContext(context);
    }
    if (!LittleFS.rename(temporary, path)) {
      LittleFS.remove(temporary);
      sendJsonMessage(request, 500, "No se pudo confirmar el recurso");
      return destroyUploadContext(context);
    }
  }
  JsonDocument response;
  response["id"] = id;
  response["mime"] = "image/bmp";
  response["format"] = "mono1";
  response["width"] = width;
  response["height"] = height;
  response["size"] = context->received;
  char crc[9];
  snprintf(crc, sizeof(crc), "%08lx", static_cast<unsigned long>(crc32Of(context->buffer, context->received)));
  response["crc32"] = crc;
  response["url"] = String("/api/assets/") + id;
  String payload;
  serializeJson(response, payload);
  destroyUploadContext(context);
  request->send(201, "application/json; charset=utf-8", payload);
}

void handleAssetGet(AsyncWebServerRequest *request) {
  const String path = resourcePath("assets", resourceIdFromRequest(request, "/api/assets/"));
  if (!LittleFS.exists(path)) return sendJsonMessage(request, 404, "Recurso no encontrado");
  AsyncWebServerResponse *response = request->beginResponse(LittleFS, path, "image/bmp", false);
  response->addHeader("Cache-Control", "public, max-age=31536000, immutable");
  request->send(response);
}

size_t folderBytes(const char *path) {
  File directory = LittleFS.open(path);
  if (!directory || !directory.isDirectory()) return 0;
  size_t total = 0;
  File entry = directory.openNextFile();
  while (entry) {
    total += entry.size();
    entry.close();
    entry = directory.openNextFile();
  }
  directory.close();
  return total;
}

void handleStorage(AsyncWebServerRequest *request) {
  JsonDocument document;
  const size_t total = LittleFS.totalBytes();
  const size_t used = LittleFS.usedBytes();
  document["total"] = total;
  document["used"] = used;
  document["free"] = total - used;
  document["reserved"] = STORAGE_RESERVE;
  document["available"] = total > used + STORAGE_RESERVE ? total - used - STORAGE_RESERVE : 0;
  File project = LittleFS.open(PROJECT_FILE, "r");
  document["projectBytes"] = project ? project.size() : 0;
  if (project) project.close();
  document["assetsBytes"] = folderBytes("/assets");
  document["framesBytes"] = folderBytes("/frames");
  document["cacheBytes"] = folderBytes("/weather") + folderBytes("/holidays");
  String payload;
  serializeJson(document, payload);
  request->send(200, "application/json; charset=utf-8", payload);
}

uint8_t *allocateBmpBuffer() {
  uint8_t *buffer = static_cast<uint8_t *>(
      heap_caps_malloc(AppConfig::BMP_FILE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    buffer = static_cast<uint8_t *>(heap_caps_malloc(AppConfig::BMP_FILE_SIZE, MALLOC_CAP_8BIT));
  }
  return buffer;
}

void destroyUploadContext(UploadContext *context) {
  if (context == nullptr) return;
  if (context->buffer != nullptr) heap_caps_free(context->buffer);
  delete context;
}

void setUploadError(UploadContext *context, int code, const char *message) {
  if (context == nullptr || context->errorCode != 0) return;
  context->errorCode = code;
  strlcpy(context->errorMessage, message, sizeof(context->errorMessage));
  Serial.printf("[HTTP][ERROR] %d: %s\n", code, message);
}

void handleDisplayBody(AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t index, size_t total) {
  auto *context = static_cast<UploadContext *>(request->_tempObject);

  if (index == 0) {
    context = new (std::nothrow) UploadContext();
    request->_tempObject = context;
    if (context == nullptr) return;
    if (request->hasHeader("X-Page-Id")) {
      const String requestedPage = request->getHeader("X-Page-Id")->value();
      if (requestedPage.length() < sizeof(activePageId)) strlcpy(activePageId, requestedPage.c_str(), sizeof(activePageId));
      runtimeInvert = request->hasHeader("X-Runtime-Invert") && request->getHeader("X-Runtime-Invert")->value() == "1";
      runtimeBlank = request->hasHeader("X-Runtime-Blank") && request->getHeader("X-Runtime-Blank")->value() == "1";
      persistRuntimeState();
      loadLiveConfiguration();
    }
    // ESPAsyncWebServer libera _tempObject con free(). Como este contexto usa
    // new y contiene otro buffer, lo retiramos nosotros si el cliente corta
    // la conexión antes de que termine la petición.
    request->onDisconnect([request]() {
      auto *orphan = static_cast<UploadContext *>(request->_tempObject);
      request->_tempObject = nullptr;
      destroyUploadContext(orphan);
    });

    if (!displayReady) {
      setUploadError(context, 500, "La pantalla no está inicializada");
    } else if (isBusy()) {
      setUploadError(context, 409, "Ya hay una actualización en curso");
    } else if (!request->contentType().startsWith("image/bmp")) {
      setUploadError(context, 400, "Content-Type debe ser image/bmp");
    } else if (total != AppConfig::BMP_FILE_SIZE) {
      setUploadError(context, 400, "El BMP debe ocupar exactamente 48062 bytes");
    } else {
      context->buffer = allocateBmpBuffer();
      if (context->buffer == nullptr) setUploadError(context, 500, "No hay memoria para recibir la imagen");
    }
  }

  context = static_cast<UploadContext *>(request->_tempObject);
  if (context == nullptr || context->errorCode != 0 || context->buffer == nullptr) return;

  if (index + length > AppConfig::BMP_FILE_SIZE) {
    setUploadError(context, 400, "El cuerpo de la petición es demasiado grande");
    return;
  }

  memcpy(context->buffer + index, data, length);
  context->received = index + length;
}

void finishDisplayUpload(AsyncWebServerRequest *request) {
  auto *context = static_cast<UploadContext *>(request->_tempObject);
  request->_tempObject = nullptr;

  if (context == nullptr) {
    sendJsonMessage(request, 500, "No se pudo reservar memoria para la petición");
    return;
  }

  if (context->errorCode != 0) {
    sendJsonMessage(request, context->errorCode, context->errorMessage);
    destroyUploadContext(context);
    return;
  }

  if (context->received != AppConfig::BMP_FILE_SIZE || !validateBmp(context->buffer, context->received)) {
    sendJsonMessage(request, 400, "BMP no válido: se requiere 800x480, monocromo y sin compresión");
    destroyUploadContext(context);
    return;
  }

  uint32_t acceptedJob = 0;
  bool accepted = false;
  portENTER_CRITICAL(&stateMux);
  if (pendingBmp == nullptr && displayState != DisplayState::Queued && displayState != DisplayState::Updating) {
    pendingBmp = context->buffer;
    context->buffer = nullptr;
    displayState = DisplayState::Queued;
    strlcpy(stateMessage, "Actualización en cola", sizeof(stateMessage));
    acceptedJob = ++currentJob;
    accepted = true;
  }
  portEXIT_CRITICAL(&stateMux);

  destroyUploadContext(context);

  if (!accepted) {
    sendJsonMessage(request, 409, "Ya hay una actualización en curso");
    return;
  }

  Serial.printf("[HTTP] BMP válido recibido: trabajo %lu, %u bytes, refresco=COMPLETO\n",
                static_cast<unsigned long>(acceptedJob), AppConfig::BMP_FILE_SIZE);
  sendJsonMessage(request, 202, "Actualización aceptada", acceptedJob);
}

void logHardwareState(const char *prefix) {
  Serial.printf("[DIAG][%s] uptime=%lums ready=%s PWR_EN(GPIO%u)=%d BUSY(GPIO%u)=%d "
                "KEY1(GPIO%u)=%d KEY2(GPIO%u)=%d KEY3(GPIO%u)=%d busy_changes=%lu heap=%u psram=%u\n",
                prefix, static_cast<unsigned long>(millis()), displayReady ? "SI" : "NO",
                AppConfig::EPD_ENABLE, digitalRead(AppConfig::EPD_ENABLE), AppConfig::EPD_BUSY,
                digitalRead(AppConfig::EPD_BUSY), AppConfig::KEY1, digitalRead(AppConfig::KEY1),
                AppConfig::KEY2, digitalRead(AppConfig::KEY2), AppConfig::KEY3,
                digitalRead(AppConfig::KEY3), static_cast<unsigned long>(busyTransitions),
                ESP.getFreeHeap(), ESP.getFreePsram());
}

bool pushFramebuffer(const char *source) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    const uint32_t transitionsBefore = busyTransitions;
    display.setRotation(displayRotation == 180 ? 180 : 0);
    int result = display.writePlane(PLANE_DUPLICATE, !runtimeBlank && (globalInvert != runtimeInvert));
    if (result == BBEP_SUCCESS) result = display.refresh(REFRESH_FULL, true);
    const uint32_t transitionDelta = busyTransitions - transitionsBefore;
    Serial.printf("[EPD][%s] FULL intento=%d rc=%d cambios_BUSY=%lu\n", source, attempt + 1, result,
                  static_cast<unsigned long>(transitionDelta));
    if (result == BBEP_SUCCESS && transitionDelta > 0) {
      lastRefreshAt = time(nullptr);
      return true;
    }
    if (attempt == 0) {
      Serial.println("[EPD][WARN] Reinicializando alimentación del panel para un único reintento");
      digitalWrite(AppConfig::EPD_ENABLE, LOW);
      delay(80);
      digitalWrite(AppConfig::EPD_ENABLE, HIGH);
      delay(40);
      display.setPanelType(EP75_800x480);
      display.initIO(AppConfig::EPD_DC, AppConfig::EPD_RST, AppConfig::EPD_BUSY, AppConfig::EPD_CS,
                     AppConfig::EPD_MOSI, AppConfig::EPD_SCK, AppConfig::EPD_SPI_SPEED);
    }
  }
  Serial.println("[EPD][ERROR] El panel no respondió después del reintento único");
  return false;
}

uint8_t *loadPersistedDisplay() {
  const String framePath = activeFrameId[0] ? String("/frames/") + activeFrameId + ".bmp" : String(DISPLAY_FILE);
  if (!storageReady || !LittleFS.exists(framePath)) return nullptr;
  File file = LittleFS.open(framePath, "r");
  if (!file || file.size() != AppConfig::BMP_FILE_SIZE) {
    if (file) file.close();
    return nullptr;
  }
  uint8_t *bmp = allocateBmpBuffer();
  if (bmp == nullptr) {
    file.close();
    return nullptr;
  }
  const size_t read = file.read(bmp, AppConfig::BMP_FILE_SIZE);
  file.close();
  if (read != AppConfig::BMP_FILE_SIZE || !validateBmp(bmp, read)) {
    heap_caps_free(bmp);
    return nullptr;
  }
  return bmp;
}

bool persistDisplay(const uint8_t *bmp) {
  if (!storageReady || bmp == nullptr) return false;
  LittleFS.remove(DISPLAY_TEMP_FILE);
  File file = LittleFS.open(DISPLAY_TEMP_FILE, "w");
  if (!file) return false;
  const size_t written = file.write(bmp, AppConfig::BMP_FILE_SIZE);
  file.flush();
  file.close();
  if (written != AppConfig::BMP_FILE_SIZE) {
    LittleFS.remove(DISPLAY_TEMP_FILE);
    return false;
  }
  LittleFS.remove(DISPLAY_FILE);
  if (!LittleFS.rename(DISPLAY_TEMP_FILE, DISPLAY_FILE)) {
    LittleFS.remove(DISPLAY_TEMP_FILE);
    return false;
  }
  return true;
}

const uint8_t *clockFont(int size) {
  if (size >= 40) return Roboto_Black_40;
  if (size >= 32) return Roboto_Black_32;
  if (size >= 24) return Roboto_Black_24;
  if (size >= 16) return Roboto_Black_16;
  return Roboto_Black_10;
}

void drawLiveClock(const LiveClockRegion &clock, const struct tm &local) {
  const int background = clock.inverted ? BBEP_BLACK : BBEP_WHITE;
  const int foreground = clock.inverted ? BBEP_WHITE : BBEP_BLACK;
  display.fillRect(clock.x, clock.y, clock.width, clock.height, background);
  if (clock.border) display.drawRect(clock.x, clock.y, clock.width, clock.height, foreground);

  static const char *WEEKDAYS[] = {"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
  static const char *MONTHS[] = {"enero", "febrero", "marzo", "abril", "mayo", "junio",
                                 "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
  static const char *SHORT_MONTHS[] = {"ene", "feb", "mar", "abr", "may", "jun",
                                       "jul", "ago", "sept", "oct", "nov", "dic"};
  String lines[3];
  size_t count = 0;
  char buffer[48];
  if (clock.showTime) {
    strftime(buffer, sizeof(buffer), clock.twelveHour ? "%I:%M %p" : "%H:%M", &local);
    lines[count++] = buffer;
  }
  if (clock.showWeekday) lines[count++] = WEEKDAYS[local.tm_wday];
  if (clock.showDate) {
    if (clock.longDate) {
      snprintf(buffer, sizeof(buffer), "%d de %s de %d", local.tm_mday, MONTHS[local.tm_mon], local.tm_year + 1900);
    } else {
      // Intl.DateTimeFormat(es-ES, { day: '2-digit', month: 'short' })
      // es el formato que usa el editor para dateStyle=short.
      snprintf(buffer, sizeof(buffer), "%02d %s", local.tm_mday, SHORT_MONTHS[local.tm_mon]);
    }
    lines[count++] = buffer;
  }
  if (count == 0) return;

  const int padding = 8;
  const int availableWidth = max(1, clock.width - padding * 2);
  const int availableHeight = max(1, clock.height - padding * 2);
  const int sizes[] = {40, 32, 24, 16};
  int fontSize = 16;
  int chosenGap = 2;
  for (int candidate : sizes) {
    display.setFont(clockFont(candidate));
    bool fits = true;
    int measuredHeight = 0;
    for (size_t index = 0; index < count; ++index) {
      BB_RECT bounds{};
      // getStringBox incluye la posición actual del cursor en x/y. Medimos
      // siempre desde el origen para que esa posición no contamine el layout.
      display.setCursor(0, 0);
      display.getStringBox(lines[index], &bounds);
      if (bounds.w > availableWidth || bounds.h <= 0) fits = false;
      measuredHeight += bounds.h;
    }
    const int gap = max(2, candidate / 8);
    measuredHeight += gap * max(0, static_cast<int>(count) - 1);
    if (measuredHeight > availableHeight) fits = false;
    if (fits) {
      fontSize = candidate;
      chosenGap = gap;
      break;
    }
  }

  display.setFont(clockFont(fontSize));
  display.setTextColor(foreground, background);
  BB_RECT measured[3]{};
  int blockHeight = chosenGap * max(0, static_cast<int>(count) - 1);
  for (size_t index = 0; index < count; ++index) {
    display.setCursor(0, 0);
    display.getStringBox(lines[index], &measured[index]);
    blockHeight += measured[index].h;
  }
  int top = clock.y + max(padding, (clock.height - blockHeight) / 2);
  for (size_t index = 0; index < count; ++index) {
    const BB_RECT &bounds = measured[index];
    const int glyphLeft = clock.x + max(padding, (clock.width - bounds.w) / 2);
    const int cursorX = glyphLeft;
    const int baseline = top - bounds.y;
    display.setCursor(cursorX, baseline);
    display.print(lines[index]);
    top += bounds.h + chosenGap;
  }
}

struct WidgetRegion {
  int x, y, width, height;
  int foreground, background;
};

WidgetRegion widgetRegion(JsonObjectConst widget) {
  WidgetRegion region{};
  region.x = constrain(widget["x"].as<int>() * 40, 0, 799);
  region.y = constrain(widget["y"].as<int>() * 40, 0, 479);
  region.width = constrain(widget["w"].as<int>() * 40, 1, 800 - region.x);
  region.height = constrain(widget["h"].as<int>() * 40, 1, 480 - region.y);
  const bool inverted = widget["config"]["invert"] | false;
  region.foreground = inverted ? BBEP_WHITE : BBEP_BLACK;
  region.background = inverted ? BBEP_BLACK : BBEP_WHITE;
  display.fillRect(region.x, region.y, region.width, region.height, region.background);
  if (widget["config"]["border"] | false) display.drawRect(region.x, region.y, region.width, region.height, region.foreground);
  return region;
}

void drawCentered(const String &text, int centerX, int baseline, int foreground, int background, int fontSize = 16) {
  display.setFont(clockFont(fontSize));
  display.setTextColor(foreground, background);
  BB_RECT bounds{};
  display.setCursor(0, 0);
  display.getStringBox(text, &bounds);
  display.setCursor(centerX - bounds.w / 2, baseline);
  display.print(text);
}

void measureTextAtOrigin(const String &text, BB_RECT &bounds) {
  display.setCursor(0, 0);
  display.getStringBox(text, &bounds);
}

void drawFittedLeft(String text, int x, int centerY, int maxWidth, int foreground, int background) {
  display.setFont(Roboto_Black_10);
  BB_RECT bounds{};
  measureTextAtOrigin(text, bounds);
  if (bounds.w > maxWidth) {
    display.setFont(FONT_8x8);
    measureTextAtOrigin(text, bounds);
  }
  if (bounds.w > maxWidth) {
    const String suffix = "...";
    do {
      size_t glyphStart = text.length() - 1;
      while (glyphStart > 0 && (static_cast<uint8_t>(text[glyphStart]) & 0xc0) == 0x80) --glyphStart;
      text.remove(glyphStart);
      measureTextAtOrigin(text + suffix, bounds);
    } while (text.length() && bounds.w > maxWidth);
    text += suffix;
    measureTextAtOrigin(text, bounds);
  }
  display.setTextColor(foreground, background);
  const int cursorY = centerY - (bounds.y + bounds.h / 2);
  display.setCursor(x, cursorY);
  display.print(text);
}

void drawWeatherSymbol(int code, int centerX, int centerY, int size, int foreground, int background) {
  (void)background;
  const int radius = max(3, size / 7);
  if (code == 800) {
    const int sunRadius = max(4, size / 5);
    display.fillCircle(centerX, centerY, sunRadius, foreground);
    const int rayStart = sunRadius + 3;
    const int rayEnd = min(size / 2, rayStart + max(3, size / 10));
    display.drawLine(centerX - rayEnd, centerY, centerX - rayStart, centerY, foreground);
    display.drawLine(centerX + rayStart, centerY, centerX + rayEnd, centerY, foreground);
    display.drawLine(centerX, centerY - rayEnd, centerX, centerY - rayStart, foreground);
    display.drawLine(centerX, centerY + rayStart, centerX, centerY + rayEnd, foreground);
    return;
  }

  // Símbolo monocromo equivalente al icono de nube del editor.
  const int cloudY = centerY + size / 12;
  display.fillCircle(centerX - size / 5, cloudY, radius, foreground);
  display.fillCircle(centerX, cloudY - radius, radius + 2, foreground);
  display.fillCircle(centerX + size / 5, cloudY, radius + 1, foreground);
  display.fillRect(centerX - size / 3, cloudY, size * 2 / 3, radius + 3, foreground);
  if (code >= 200 && code < 700) {
    const int rainY = cloudY + radius + 8;
    for (int offset = -size / 5; offset <= size / 5; offset += max(5, size / 5)) {
      display.drawLine(centerX + offset, rainY, centerX + offset - 3, rainY + max(4, size / 10), foreground);
    }
  }
}

static const uint16_t WEATHER_ICON_TEMPERATURE[16] = {
    0x0700, 0x089f, 0x0880, 0x088e, 0x0880, 0x0880, 0x0880, 0x0880,
    0x1040, 0x2020, 0x2020, 0x2020, 0x1040, 0x0f80, 0x0000, 0x0000};
static const uint16_t WEATHER_ICON_FEELS[16] = {
    0x0700, 0x0880, 0x0a80, 0x0a80, 0x0a80, 0x0a80, 0x0a80, 0x0b80,
    0x1040, 0x2720, 0x2fa0, 0x2fa0, 0x1740, 0x0f80, 0x0000, 0x0000};
static const uint16_t WEATHER_ICON_HUMIDITY[16] = {
    0x0100, 0x0380, 0x07c0, 0x0ee0, 0x1c70, 0x3838, 0x3018, 0x3298,
    0x3018, 0x3458, 0x3118, 0x1830, 0x0fe0, 0x0380, 0x0000, 0x0000};
static const uint16_t WEATHER_ICON_WIND[16] = {
    0x0038, 0x000c, 0x000c, 0x3ff8, 0x0000, 0x3ffc, 0x0003, 0x0003,
    0x3ffc, 0x0000, 0x3fe0, 0x0070, 0x0070, 0x3fc0, 0x0000, 0x0000};
static const uint16_t WEATHER_ICON_RAIN[16] = {
    0x0100, 0x0380, 0x07c0, 0x0fe0, 0x1ff0, 0x3ff8, 0x3ff8, 0x3ff8,
    0x1ff0, 0x0fe0, 0x07c0, 0x0380, 0x0100, 0x0000, 0x0000, 0x0000};
static const uint16_t WEATHER_ICON_CLOUD[16] = {
    0x0000, 0x03e0, 0x0ff8, 0x1ffc, 0x3ffc, 0x3ffe, 0x7fff, 0x7fff,
    0x3ffe, 0x1ffc, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
static const uint16_t WEATHER_ICON_SUNRISE[16] = {
    0x0100, 0x1110, 0x0920, 0x07c0, 0x0fe0, 0x1ff0, 0x3ff8, 0x0000,
    0x3ffc, 0x0000, 0x3ffc, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
static const uint16_t WEATHER_ICON_LOCATION[16] = {
    0x07e0, 0x1ff8, 0x3ffc, 0x3ffc, 0x3c3c, 0x3c3c, 0x3ffc, 0x1ff8,
    0x0ff0, 0x07e0, 0x03c0, 0x0180, 0x0180, 0x0000, 0x0000, 0x0000};

void drawScaledWeatherIcon(const uint16_t *rows, int centerX, int centerY, int size, int color) {
  size = constrain(size, 8, 24);
  const int left = centerX - size / 2;
  const int top = centerY - size / 2;
  for (int y = 0; y < size; ++y) {
    const uint16_t row = rows[y * 16 / size];
    for (int x = 0; x < size; ++x) {
      if (row & (0x8000U >> (x * 16 / size))) display.drawPixel(left + x, top + y, color);
    }
  }
}

void drawWeatherMetricIcon(const char *kind, int x, int y, int size, int color) {
  const uint16_t *icon = WEATHER_ICON_RAIN;
  if (!strcmp(kind, "temperature")) icon = WEATHER_ICON_TEMPERATURE;
  else if (!strcmp(kind, "feels")) icon = WEATHER_ICON_FEELS;
  else if (!strcmp(kind, "humidity")) icon = WEATHER_ICON_HUMIDITY;
  else if (!strcmp(kind, "wind")) icon = WEATHER_ICON_WIND;
  else if (!strcmp(kind, "cloud")) icon = WEATHER_ICON_CLOUD;
  else if (!strcmp(kind, "sunrise") || !strcmp(kind, "sunset")) icon = WEATHER_ICON_SUNRISE;
  else if (!strcmp(kind, "location")) icon = WEATHER_ICON_LOCATION;
  drawScaledWeatherIcon(icon, x, y, size, color);
}

String compactDecimal(double value) {
  const double rounded = round(value);
  return fabs(value - rounded) < 0.05 ? String(static_cast<int>(rounded)) : String(value, 1);
}

String weatherCachePath(const String &widgetId) {
  const String hash = sha256Hex(reinterpret_cast<const uint8_t *>(widgetId.c_str()), widgetId.length());
  return String("/weather/") + hash.substring(0, 24) + ".json";
}

bool refreshWeatherCaches() {
  if (!Connectivity::connected() || !Connectivity::weatherConfigured() || !LittleFS.exists(PROJECT_FILE)) return false;
  File file = LittleFS.open(PROJECT_FILE, "r");
  JsonDocument project;
  if (!file || deserializeJson(project, file)) { if (file) file.close(); return false; }
  file.close();
  bool updated = false;
  for (JsonObjectConst page : project["pages"].as<JsonArrayConst>()) {
    for (JsonObjectConst widget : page["widgets"].as<JsonArrayConst>()) {
      if (String(widget["type"] | "") != "weather") continue;
      JsonObjectConst config = widget["config"];
      if (!config["latitude"].is<double>() || !config["longitude"].is<double>()) continue;
      String payload, error;
      const bool fahrenheit = String(config["units"] | "celsius") == "fahrenheit";
      if (!Connectivity::fetchWeather(config["latitude"].as<double>(), config["longitude"].as<double>(), fahrenheit,
                                      constrain(config["forecastDays"] | 3, 1, 5), payload, error, false)) {
        Serial.printf("[WEATHER][WARN] %s\n", error.c_str());
        continue;
      }
      const String path = weatherCachePath(String(widget["id"] | ""));
      const String temporary = path + ".upload";
      if (!hasStorageFor(payload.length())) continue;
      LittleFS.remove(temporary);
      File cache = LittleFS.open(temporary, "w");
      if (!cache || cache.print(payload) != payload.length()) { if (cache) cache.close(); LittleFS.remove(temporary); continue; }
      cache.flush(); cache.close();
      LittleFS.remove(path);
      if (LittleFS.rename(temporary, path)) updated = true;
    }
  }
  return updated;
}

void drawWeatherWidget(JsonObjectConst widget) {
  const WidgetRegion region = widgetRegion(widget);
  JsonObjectConst config = widget["config"];
  JsonDocument cachedDocument;
  const String cachePath = weatherCachePath(String(widget["id"] | ""));
  if (LittleFS.exists(cachePath)) {
    File cache = LittleFS.open(cachePath, "r");
    if (cache) deserializeJson(cachedDocument, cache);
    if (cache) cache.close();
  }
  JsonObjectConst weather = cachedDocument["temperature"].isNull() ? config["weather"].as<JsonObjectConst>()
                                                                  : cachedDocument.as<JsonObjectConst>();
  const int centerX = region.x + region.width / 2;
  if (weather.isNull()) {
    drawCentered("Sin datos del tiempo", centerX, region.y + region.height / 2, region.foreground, region.background, 16);
    return;
  }
  const bool fahrenheit = String(config["units"] | "celsius") == "fahrenheit";
  if (String(config["view"] | "current") == "forecast" && weather["daily"].is<JsonArrayConst>()) {
    JsonArrayConst days = weather["daily"].as<JsonArrayConst>();
    const int count = min(static_cast<int>(days.size()), constrain(config["forecastDays"] | 3, 1, 5));
    if (count > 0) {
      const int columnWidth = region.width / count;
      for (int index = 0; index < count; ++index) {
        JsonObjectConst day = days[index];
        const int x = region.x + index * columnWidth + columnWidth / 2;
        String date = day["date"] | "";
        if (date.length() >= 10) date = date.substring(8, 10) + "/" + date.substring(5, 7);
        drawCentered(date, x, region.y + 24, region.foreground, region.background, 16);
        if (config["showIcon"] | true) {
          drawWeatherSymbol(day["code"] | 0, x, region.y + min(region.height - 48, 62),
                            min(columnWidth - 8, 48), region.foreground, region.background);
        }
        const String range = String(static_cast<int>(round(day["max"].as<double>()))) + "°/" +
                             String(static_cast<int>(round(day["min"].as<double>()))) + "°";
        drawCentered(range, x, region.y + min(region.height - 12, 100), region.foreground, region.background, 16);
        String condition = day["condition"] | "";
        if (condition.length() > 12) condition = condition.substring(0, 12);
        if (region.height >= 130) drawCentered(condition, x, region.y + min(region.height - 8, 124), region.foreground, region.background, 16);
      }
      return;
    }
  }

  const int padding = max(4, config["padding"] | 12);
  const bool showTitle = config["showTitle"] | false;
  const int titleHeight = showTitle ? max(22, region.height * 12 / 100) : 0;
  if (showTitle) {
    String title = config["title"] | "Tiempo";
    drawFittedLeft(title, region.x + padding, region.y + titleHeight / 2,
                   max(1, region.width - padding - 2), region.foreground, region.background);
  }

  const int areaX = region.x + padding;
  const int areaY = region.y + titleHeight + max(1, padding / 4);
  const int areaWidth = max(1, region.width - padding * 2);
  const int areaHeight = max(1, region.height - titleHeight - padding * 5 / 4);
  const bool showIcon = config["showIcon"] | true;
  const int iconSize = showIcon ? min(areaWidth, areaHeight) * 38 / 100 : 0;
  if (showIcon) {
    drawWeatherSymbol(weather["code"] | 0, areaX + iconSize / 2, areaY + areaHeight / 2,
                      iconSize, region.foreground, region.background);
  }

  struct WeatherMetric { const char *kind; String value; };
  WeatherMetric metrics[9];
  size_t metricCount = 0;
  const String temperatureUnit = fahrenheit ? "°F" : "°C";
  const String windUnit = fahrenheit ? "mph" : "km/h";
  const String rainUnit = fahrenheit ? "in" : "mm";
  auto addMetric = [&](const char *kind, const String &value) {
    if (metricCount < sizeof(metrics) / sizeof(metrics[0])) metrics[metricCount++] = {kind, value};
  };
  addMetric("temperature", String(static_cast<int>(round(weather["temperature"].as<double>()))) + temperatureUnit);
  if ((config["showFeelsLike"] | false) && !weather["apparentTemperature"].isNull())
    addMetric("feels", String(static_cast<int>(round(weather["apparentTemperature"].as<double>()))) + temperatureUnit);
  if ((config["showHumidity"] | false) && !weather["humidity"].isNull())
    addMetric("humidity", String(static_cast<int>(round(weather["humidity"].as<double>()))) + "%");
  if ((config["showWind"] | false) && !weather["wind"].isNull())
    addMetric("wind", String(static_cast<int>(round(weather["wind"].as<double>()))) + " " + windUnit);
  if ((config["showPrecipitation"] | false) && !weather["precipitation"].isNull())
    addMetric("rain", compactDecimal(weather["precipitation"].as<double>()) + rainUnit);
  if ((config["showCloudCover"] | false) && !weather["cloudCover"].isNull())
    addMetric("cloud", String(static_cast<int>(round(weather["cloudCover"].as<double>()))) + "%");
  JsonArrayConst daily = weather["daily"].as<JsonArrayConst>();
  JsonObjectConst today = daily.isNull() || daily.size() == 0 ? JsonObjectConst() : daily[0].as<JsonObjectConst>();
  if ((config["showSun"] | false) && !today.isNull()) {
    String sunrise = today["sunrise"] | "";
    String sunset = today["sunset"] | "";
    if (sunrise.length() >= 16) addMetric("sunrise", sunrise.substring(11, 16));
    if (sunset.length() >= 16) addMetric("sunset", sunset.substring(11, 16));
  }
  if ((config["showLocation"] | false) && !String(config["location"] | "").isEmpty()) {
    String location = config["location"] | "";
    const int comma = location.indexOf(',');
    addMetric("location", comma > 0 ? location.substring(0, comma) : location);
  }

  if (metricCount == 0) return;
  const int metricX = areaX + (showIcon ? iconSize + padding * 3 / 5 : 0);
  const int metricWidth = max(1, areaWidth - (metricX - areaX));
  const int columns = metricCount == 1 || metricWidth < 170 ? 1 : 2;
  const int rows = (static_cast<int>(metricCount) + columns - 1) / columns;
  const int cellWidth = max(1, metricWidth / columns);
  const int cellHeight = max(1, areaHeight / rows);
  for (size_t index = 0; index < metricCount; ++index) {
    const int column = index % columns;
    const int row = index / columns;
    const int cellX = metricX + column * cellWidth;
    const int cellY = areaY + row * cellHeight;
    const int smallIcon = constrain(min(cellHeight / 2, cellWidth / 5), 10, 20);
    drawWeatherMetricIcon(metrics[index].kind, cellX + smallIcon / 2 + 2,
                          cellY + cellHeight / 2, smallIcon, region.foreground);
    const int textX = cellX + smallIcon + 7;
    drawFittedLeft(metrics[index].value, textX, cellY + cellHeight / 2,
                   max(1, cellX + cellWidth - textX - 2), region.foreground, region.background);
  }
}

int daysInMonth(int year, int month) {
  static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return DAYS[month];
}

String holidayCachePath(int year, const String &region) {
  String safe = region;
  safe.replace("/", "_");
  return String("/holidays/") + year + "_" + safe + ".json";
}

void refreshHolidayCaches(const struct tm &local) {
  if (!Connectivity::connected() || !LittleFS.exists(PROJECT_FILE)) return;
  File file = LittleFS.open(PROJECT_FILE, "r");
  JsonDocument project;
  if (!file || deserializeJson(project, file)) { if (file) file.close(); return; }
  file.close();
  JsonObjectConst spain = project["agenda"]["holidays"]["spain"];
  if (!(spain["enabled"] | false)) return;
  const String region = spain["region"] | "";
  for (int year = local.tm_year + 1900; year <= local.tm_year + 1901; ++year) {
    String payload, error;
    if (!Connectivity::fetchHolidays(year, region, payload, error)) {
      Serial.printf("[HOLIDAYS][WARN] %s\n", error.c_str());
      continue;
    }
    const String path = holidayCachePath(year, region);
    const String temporary = path + ".upload";
    if (!hasStorageFor(payload.length())) continue;
    LittleFS.remove(temporary);
    File cache = LittleFS.open(temporary, "w");
    if (!cache || cache.print(payload) != payload.length()) { if (cache) cache.close(); LittleFS.remove(temporary); continue; }
    cache.flush(); cache.close();
    LittleFS.remove(path);
    LittleFS.rename(temporary, path);
  }
}

String holidayForDate(JsonObjectConst agenda, const String &date, int year) {
  for (JsonObjectConst holiday : agenda["holidays"]["manual"].as<JsonArrayConst>()) {
    if (String(holiday["date"] | "") == date) return String(holiday["title"] | "Festivo");
  }
  JsonObjectConst spain = agenda["holidays"]["spain"];
  if (!(spain["enabled"] | false)) return String();
  const String path = holidayCachePath(year, String(spain["region"] | ""));
  File file = LittleFS.open(path, "r");
  JsonDocument cache;
  if (!file || deserializeJson(cache, file)) { if (file) file.close(); return String(); }
  file.close();
  for (JsonObjectConst holiday : cache["holidays"].as<JsonArrayConst>()) {
    if (String(holiday["date"] | "") == date) return String(holiday["title"] | "Festivo");
  }
  return String();
}

void drawCalendarWidget(JsonObjectConst widget, JsonObjectConst agenda, const struct tm &local) {
  const WidgetRegion region = widgetRegion(widget);
  static const char *MONTHS[] = {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
  const int padding = 6;
  const int centerX = region.x + region.width / 2;
  drawCentered(String(MONTHS[local.tm_mon]) + " " + String(local.tm_year + 1900), centerX, region.y + 24,
               region.foreground, region.background, 16);
  const int cellWidth = max(12, (region.width - padding * 2) / 7);
  const int cellHeight = max(13, (region.height - 48) / 7);
  static const char *WEEK[] = {"L", "M", "X", "J", "V", "S", "D"};
  for (int column = 0; column < 7; ++column) drawCentered(WEEK[column], region.x + padding + column * cellWidth + cellWidth / 2,
                                                          region.y + 43, region.foreground, region.background, 16);
  struct tm first = local;
  first.tm_mday = 1; first.tm_hour = 12; first.tm_min = 0; first.tm_sec = 0;
  mktime(&first);
  const int offset = (first.tm_wday + 6) % 7;
  const int total = daysInMonth(local.tm_year + 1900, local.tm_mon);
  for (int day = 1; day <= total; ++day) {
    const int index = offset + day - 1;
    const int x = region.x + padding + (index % 7) * cellWidth + cellWidth / 2;
    const int y = region.y + 43 + (index / 7 + 1) * cellHeight;
    if (day == local.tm_mday) display.fillCircle(x, y - 5, min(9, cellHeight / 2), region.foreground);
    drawCentered(String(day), x, y, day == local.tm_mday ? region.background : region.foreground,
                 day == local.tm_mday ? region.foreground : region.background, 16);
  }
  char today[11];
  strftime(today, sizeof(today), "%Y-%m-%d", &local);
  const String holiday = holidayForDate(agenda, today, local.tm_year + 1900);
  if (!holiday.isEmpty()) drawCentered(holiday, centerX, region.y + region.height - 5,
                                       region.foreground, region.background, 16);
}

void drawEventsWidget(JsonObjectConst widget, JsonObjectConst agenda, const struct tm &local) {
  const WidgetRegion region = widgetRegion(widget);
  JsonObjectConst config = widget["config"];
  const int maximum = constrain(config["maxEvents"] | 6, 1, 10);
  char today[11];
  strftime(today, sizeof(today), "%Y-%m-%d", &local);
  int row = 0;
  for (JsonObjectConst event : agenda["events"].as<JsonArrayConst>()) {
    if (row >= maximum || String(event["date"] | "") < today) continue;
    String label;
    if ((config["showTimes"] | true) && !String(event["start"] | "").isEmpty()) label += String(event["start"] | "") + " ";
    label += String(event["title"] | "Evento");
    display.setFont(Roboto_Black_16);
    display.setTextColor(region.foreground, region.background);
    display.setCursor(region.x + 8, region.y + 22 + row * 22);
    display.print(label);
    ++row;
  }
  if (row == 0) drawCentered("Sin próximos eventos", region.x + region.width / 2, region.y + region.height / 2,
                             region.foreground, region.background, 16);
}

bool prepareLiveFrame(const uint8_t *baseBmp, time_t timestamp, uint8_t layers) {
  display.setPlane(PLANE_0);
  display.fillScreen(BBEP_WHITE);
  if (runtimeBlank) return true;
  int result = display.loadBMP(baseBmp, 0, 0, BBEP_WHITE, BBEP_BLACK);
  if (result != BBEP_SUCCESS) return false;
  struct tm local {};
  localtime_r(&timestamp, &local);
  if (layers & DYNAMIC_CLOCK) {
    for (size_t index = 0; index < liveClockCount; ++index) drawLiveClock(liveClocks[index], local);
  }
  if (!(layers & (DYNAMIC_WEATHER | DYNAMIC_AGENDA))) return true;
  File projectFile = LittleFS.open(PROJECT_FILE, "r");
  JsonDocument project;
  if (projectFile && !deserializeJson(project, projectFile)) {
    for (JsonObjectConst page : project["pages"].as<JsonArrayConst>()) {
      if (String(page["id"] | "") != activePageId) continue;
      for (JsonObjectConst widget : page["widgets"].as<JsonArrayConst>()) {
        const String type = widget["type"] | "";
        if (type == "weather" && (layers & DYNAMIC_WEATHER)) drawWeatherWidget(widget);
        else if (type == "calendar" && (layers & DYNAMIC_AGENDA))
          drawCalendarWidget(widget, project["agenda"].as<JsonObjectConst>(), local);
        else if (type == "events" && (layers & DYNAMIC_AGENDA))
          drawEventsWidget(widget, project["agenda"].as<JsonObjectConst>(), local);
      }
      break;
    }
  }
  if (projectFile) projectFile.close();
  return true;
}

bool updateDynamicPage(time_t now, uint8_t layers = DYNAMIC_ALL) {
  uint8_t *baseBmp = loadPersistedDisplay();
  if (baseBmp == nullptr) return false;
  display.setRotation(displayRotation == 180 ? 180 : 0);
  int result = display.allocBuffer(false);
  if (result != BBEP_SUCCESS) {
    heap_caps_free(baseBmp);
    return false;
  }

  const bool success = prepareLiveFrame(baseBmp, now, layers) && pushFramebuffer("DINAMICO");
  display.freeBuffer();
  heap_caps_free(baseBmp);
  return success;
}

void processLiveRefresh() {
  if ((!refreshEveryMinute && !refreshEveryHour && !refreshAtMidnight) || !displayReady || isBusy() || !storageReady) return;
  const uint32_t nowMs = millis();
  if (nowMs - lastLiveRefreshCheck < 5000) return;
  lastLiveRefreshCheck = nowMs;
  const time_t now = time(nullptr);
  if (now < 1700000000) return;
  if (nextDynamicRefreshAt == 0) calculateNextDynamicRefresh(now);
  if (nextDynamicRefreshAt == 0 || now < nextDynamicRefreshAt) return;

  portENTER_CRITICAL(&stateMux);
  displayState = DisplayState::Updating;
  snprintf(stateMessage, sizeof(stateMessage), "Actualizando %s", nextDynamicReason);
  ++currentJob;
  portEXIT_CRITICAL(&stateMux);
  Serial.printf("[LIVE] Refresco completo inteligente: %s\n", nextDynamicReason);
  if (strstr(nextDynamicReason, "tiempo") != nullptr && lastBackgroundWeatherHour != now / 3600) {
    lastBackgroundWeatherHour = now / 3600;
    refreshWeatherCaches();
  }
  if (strstr(nextDynamicReason, "fecha/calendario") != nullptr) {
    struct tm local {};
    localtime_r(&now, &local);
    const int dayKey = (local.tm_year + 1900) * 1000 + local.tm_yday;
    if (dayKey != lastBackgroundHolidayDay) {
      lastBackgroundHolidayDay = dayKey;
      refreshHolidayCaches(local);
    }
  }
  uint8_t layers = 0;
  if (strstr(nextDynamicReason, "hora") != nullptr) layers |= DYNAMIC_CLOCK;
  if (strstr(nextDynamicReason, "tiempo") != nullptr) layers |= DYNAMIC_WEATHER;
  if (strstr(nextDynamicReason, "fecha/calendario") != nullptr) layers |= DYNAMIC_CLOCK | DYNAMIC_AGENDA;
  const bool success = updateDynamicPage(now, layers ? layers : DYNAMIC_CLOCK);
  calculateNextDynamicRefresh(now);
  setState(success ? DisplayState::Ready : DisplayState::Error,
           success ? "Página dinámica actualizada" : "Falló la actualización dinámica");
}

void processBackgroundData() {
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < 30000 || !Connectivity::connected()) return;
  lastCheck = millis();
  const time_t now = time(nullptr);
  if (now < 1700000000) return;
  const time_t hour = now / 3600;
  if (lastBackgroundWeatherHour == 0) {
    // El frame persistido ya contiene el render exacto del editor. En el
    // primer ciclo solo calentamos la caché; no lo sustituimos al arrancar.
    lastBackgroundWeatherHour = hour;
    refreshWeatherCaches();
  } else if (hour != lastBackgroundWeatherHour) {
    lastBackgroundWeatherHour = hour;
    if (refreshWeatherCaches() && refreshEveryHour)
      queueDynamicRefresh("Tiempo actualizado", DYNAMIC_WEATHER | (refreshEveryMinute ? DYNAMIC_CLOCK : 0));
  }
  struct tm local {};
  localtime_r(&now, &local);
  const int dayKey = (local.tm_year + 1900) * 1000 + local.tm_yday;
  if (lastBackgroundHolidayDay < 0) {
    lastBackgroundHolidayDay = dayKey;
    refreshHolidayCaches(local);
  } else if (dayKey != lastBackgroundHolidayDay) {
    lastBackgroundHolidayDay = dayKey;
    refreshHolidayCaches(local);
    if (refreshAtMidnight) queueDynamicRefresh("Festivos actualizados", DYNAMIC_CLOCK | DYNAMIC_AGENDA);
  }
}

bool updateDisplay(const uint8_t *bmp) {
  if (!displayReady || bmp == nullptr) return false;

  Serial.println("[EPD][BMP] Reservando framebuffer...");
  display.setRotation(displayRotation == 180 ? 180 : 0);
  int result = display.allocBuffer(false);
  if (result != BBEP_SUCCESS) {
    Serial.printf("[EPD][ERROR] No se pudo reservar el framebuffer: %d\n", result);
    return false;
  }

  display.setPlane(PLANE_0);
  display.fillScreen(BBEP_WHITE);
  // En el BMP monocromo generado por Studio, los bits a 1 usan la entrada 1
  // de la paleta (blanco) y los bits a 0 la entrada 0 (negro). loadBMP no lee
  // la paleta: su primer color corresponde directamente a los bits a 1.
  result = display.loadBMP(bmp, 0, 0, BBEP_WHITE, BBEP_BLACK);
  Serial.printf("[EPD][BMP] loadBMP rc=%d\n", result);
  const bool success = result == BBEP_SUCCESS && pushFramebuffer("BMP");
  display.freeBuffer();

  if (!success) {
    if (result != BBEP_SUCCESS) Serial.printf("[EPD][ERROR] loadBMP falló con código %d\n", result);
    return false;
  }
  if (!persistDisplay(bmp)) Serial.println("[EPD][WARN] No se pudo persistir el último BMP");
  return true;
}

bool updateSolidDisplay(int color) {
  if (!displayReady) return false;

  Serial.printf("[EPD][TEST] Preparando pantalla %s...\n", color == BBEP_WHITE ? "BLANCA" : "NEGRA");
  int result = display.allocBuffer(false);
  if (result != BBEP_SUCCESS) {
    Serial.printf("[EPD][ERROR] No se pudo reservar el framebuffer: %d\n", result);
    return false;
  }

  display.fillScreen(color);
  const bool success = pushFramebuffer(color == BBEP_WHITE ? "KEY1-BLANCO" : "KEY2-NEGRO");
  display.freeBuffer();

  return success;
}

bool queueSolidDisplay(int color, const char *message) {
  bool accepted = false;
  portENTER_CRITICAL(&stateMux);
  if (displayReady && pendingBmp == nullptr && pendingSolid < 0 &&
      displayState != DisplayState::Queued && displayState != DisplayState::Updating) {
    pendingSolid = static_cast<int8_t>(color);
    displayState = DisplayState::Queued;
    strlcpy(stateMessage, message, sizeof(stateMessage));
    ++currentJob;
    accepted = true;
  }
  portEXIT_CRITICAL(&stateMux);
  return accepted;
}

bool queueDynamicRefresh(const char *message, uint8_t layers) {
  bool accepted = false;
  portENTER_CRITICAL(&stateMux);
  if (displayReady && pendingBmp == nullptr && pendingSolid < 0 && displayState == DisplayState::Updating) {
    pendingDynamic = true;
    pendingDynamicLayers |= layers;
    accepted = true;
  } else if (displayReady && pendingBmp == nullptr && !pendingDynamic && pendingSolid < 0 &&
             displayState != DisplayState::Queued) {
    pendingDynamic = true;
    pendingDynamicLayers = layers;
    displayState = DisplayState::Queued;
    strlcpy(stateMessage, message, sizeof(stateMessage));
    ++currentJob;
    accepted = true;
  }
  portEXIT_CRITICAL(&stateMux);
  return accepted;
}

void persistRuntimeState() {
  JsonDocument runtime;
  runtime["activePageId"] = activePageId;
  runtime["inverted"] = runtimeInvert;
  runtime["blank"] = runtimeBlank;
  const String temporary = String(RUNTIME_FILE) + ".upload";
  LittleFS.remove(temporary);
  File file = LittleFS.open(temporary, "w");
  if (!file) return;
  serializeJson(runtime, file);
  file.flush();
  file.close();
  LittleFS.remove(RUNTIME_FILE);
  LittleFS.rename(temporary, RUNTIME_FILE);
}

void applyConfiguredButton(const char *key) {
  if (!storageReady || !LittleFS.exists(PROJECT_FILE) || isBusy()) return;
  File file = LittleFS.open(PROJECT_FILE, "r");
  JsonDocument document;
  if (!file || deserializeJson(document, file)) { if (file) file.close(); return; }
  file.close();
  JsonObjectConst button = document["device"]["buttons"][key];
  const String action = button["action"] | "none";
  JsonArrayConst pages = document["pages"].as<JsonArrayConst>();
  int current = 0;
  for (size_t index = 0; index < pages.size(); ++index) {
    if (String(pages[index]["id"] | "") == activePageId) { current = static_cast<int>(index); break; }
  }
  bool changedPage = false;
  if (action == "previous_page" && pages.size()) {
    current = (current - 1 + static_cast<int>(pages.size())) % static_cast<int>(pages.size());
    strlcpy(activePageId, String(pages[current]["id"] | "").c_str(), sizeof(activePageId));
    changedPage = true;
  } else if (action == "next_page" && pages.size()) {
    current = (current + 1) % static_cast<int>(pages.size());
    strlcpy(activePageId, String(pages[current]["id"] | "").c_str(), sizeof(activePageId));
    changedPage = true;
  } else if (action == "show_page") {
    const String target = button["pageId"] | "";
    for (JsonObjectConst page : pages) if (String(page["id"] | "") == target) {
      strlcpy(activePageId, target.c_str(), sizeof(activePageId));
      changedPage = true;
      break;
    }
  } else if (action == "toggle_invert") {
    runtimeInvert = !runtimeInvert;
  } else if (action == "blank") {
    runtimeBlank = true;
  } else if (action != "refresh") {
    return;
  }
  if (changedPage) runtimeBlank = false;
  persistRuntimeState();
  loadLiveConfiguration();
  queueDynamicRefresh("Acción de botón");
}

void pollButton(ButtonState &button, const char *key) {
  const bool reading = digitalRead(button.pin);
  const uint32_t now = millis();

  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.changedAt = now;
  }

  if (reading != button.stableState && now - button.changedAt >= AppConfig::BUTTON_DEBOUNCE_MS) {
    button.stableState = reading;
    if (reading == LOW) {
      Serial.printf("Botón GPIO%u: ejecutando %s\n", button.pin, key);
      applyConfiguredButton(key);
    }
  }
}

void setupDisplay() {
  // La placa EE04 mantiene desconectada la alimentación del panel mientras
  // PWR_EN está bajo. bb_epaper configura el bus y el UC8179, pero no conoce
  // esta línea propia de la placa Seeed.
  pinMode(AppConfig::EPD_ENABLE, OUTPUT);
  digitalWrite(AppConfig::EPD_ENABLE, HIGH);
  delay(20);
  Serial.printf("[BOOT] PWR_EN GPIO%u activado, lectura=%d\n", AppConfig::EPD_ENABLE,
                digitalRead(AppConfig::EPD_ENABLE));

  const int result = display.setPanelType(EP75_800x480);
  Serial.printf("[BOOT] setPanelType(EP75_800x480) rc=%d\n", result);
  if (result != BBEP_SUCCESS) {
    Serial.printf("No se pudo seleccionar el panel: %d\n", result);
    displayReady = false;
    setState(DisplayState::Error, "No se pudo inicializar el panel e-paper");
    return;
  }

  display.initIO(AppConfig::EPD_DC, AppConfig::EPD_RST, AppConfig::EPD_BUSY, AppConfig::EPD_CS,
                 AppConfig::EPD_MOSI, AppConfig::EPD_SCK, AppConfig::EPD_SPI_SPEED);
  attachInterrupt(digitalPinToInterrupt(AppConfig::EPD_BUSY), handleBusyChange, CHANGE);
  Serial.printf("[BOOT] SPI e-paper: SCK=%u MOSI=%u CS=%u RST=%u DC=%u BUSY=%u @ %luHz\n",
                AppConfig::EPD_SCK, AppConfig::EPD_MOSI, AppConfig::EPD_CS, AppConfig::EPD_RST,
                AppConfig::EPD_DC, AppConfig::EPD_BUSY,
                static_cast<unsigned long>(AppConfig::EPD_SPI_SPEED));
  display.setFlip180(AppConfig::DISPLAY_FLIP_180);
  display.setLightSleep(false);
  displayReady = true;
  setState(DisplayState::Ready, "Preparado");
}

void setupWebServer() {
  server.on("/", HTTP_GET, serveIndex);
  server.on("/index.html", HTTP_GET, serveIndex);
  server.on("/styles.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveCompressed(request, "text/css; charset=utf-8", WebAssets::STYLES_CSS_GZ, WebAssets::STYLES_CSS_GZ_LEN);
  });
  server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveCompressed(request, "application/javascript; charset=utf-8", WebAssets::APP_JS_GZ, WebAssets::APP_JS_GZ_LEN);
  });
  server.on("/model.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveCompressed(request, "application/javascript; charset=utf-8", WebAssets::MODEL_JS_GZ, WebAssets::MODEL_JS_GZ_LEN);
  });
  server.on("/render.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveCompressed(request, "application/javascript; charset=utf-8", WebAssets::RENDER_JS_GZ, WebAssets::RENDER_JS_GZ_LEN);
  });
  server.on("/material-icons.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveCompressed(request, "application/javascript; charset=utf-8", WebAssets::MATERIAL_ICONS_JS_GZ, WebAssets::MATERIAL_ICONS_JS_GZ_LEN);
  });
  server.on("/vendor/qrcode.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveCompressed(request, "application/javascript; charset=utf-8", WebAssets::QRCODE_JS_GZ, WebAssets::QRCODE_JS_GZ_LEN);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/project", HTTP_GET, handleProjectGet);
  server.on("/api/project", HTTP_PUT, finishProjectUpload, nullptr, handleProjectBody);
  server.on("/api/device/status", HTTP_GET, handleDeviceStatus);
  server.on("/api/device/storage", HTTP_GET, handleStorage);
  server.on("/api/display", HTTP_POST, finishDisplayUpload, nullptr, handleDisplayBody);
  server.on("/api/assets/*", HTTP_POST,
            [](AsyncWebServerRequest *request) { finishResourceUpload(request, false); }, nullptr, handleResourceBody);
  server.on("/api/assets/*", HTTP_GET, handleAssetGet);
  server.on("/api/assets/*", HTTP_DELETE, [](AsyncWebServerRequest *request) {
    const String id = resourceIdFromRequest(request, "/api/assets/");
    if (id.isEmpty()) return sendJsonMessage(request, 400, "Identificador de recurso no válido");
    if (LittleFS.exists(PROJECT_FILE)) {
      File project = LittleFS.open(PROJECT_FILE, "r");
      const String content = project ? project.readString() : String();
      if (project) project.close();
      if (content.indexOf(id) >= 0) return sendJsonMessage(request, 409, "El recurso sigue en uso");
    }
    const String path = resourcePath("assets", id);
    if (!LittleFS.exists(path)) return sendJsonMessage(request, 404, "Recurso no encontrado");
    LittleFS.remove(path);
    sendJsonMessage(request, 200, "Recurso eliminado");
  });
  server.on("/api/frames/*", HTTP_PUT,
            [](AsyncWebServerRequest *request) { finishResourceUpload(request, true); }, nullptr, handleResourceBody);
  Connectivity::registerRoutes(server);

  // Rutas habituales de detección de portales cautivos.
  const char *portalRoutes[] = {"/generate_204",          "/gen_204",       "/hotspot-detect.html",
                                "/library/test/success.html", "/connecttest.txt", "/ncsi.txt",
                                "/redirect"};
  for (const char *route : portalRoutes) server.on(route, HTTP_ANY, serveIndex);

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->url().startsWith("/api/")) {
      sendJsonMessage(request, 404, "Ruta API no encontrada");
    } else {
      serveIndex(request);
    }
  });

  server.begin();
}

void setupProjectStorage() {
  storageReady = LittleFS.begin(false);
  if (!storageReady) {
    Serial.println("[PROJECT][WARN] LittleFS no se pudo montar; inicializando la partición...");
    LittleFS.end();
    storageReady = LittleFS.begin(true);
    if (!storageReady) {
      Serial.println("[PROJECT][ERROR] LittleFS no se pudo inicializar; se usará solo el navegador");
      return;
    }
  }
  Serial.printf("[PROJECT] LittleFS listo: %u/%u bytes, proyecto=%s\n", static_cast<unsigned>(LittleFS.usedBytes()),
                static_cast<unsigned>(LittleFS.totalBytes()), LittleFS.exists(PROJECT_FILE) ? "SI" : "NO");
  LittleFS.mkdir("/assets");
  LittleFS.mkdir("/frames");
  LittleFS.mkdir("/weather");
  LittleFS.mkdir("/holidays");
  loadLiveConfiguration();
}

void setupAccessPoint() {
  Connectivity::begin();
  dnsServer.start(AppConfig::DNS_PORT, "*", AppConfig::AP_IP);
}

void processPendingDisplay() {
  uint8_t *work = nullptr;
  bool dynamic = false;
  uint8_t dynamicLayers = 0;
  int solid = -1;

  portENTER_CRITICAL(&stateMux);
  if (pendingBmp != nullptr && displayState == DisplayState::Queued) {
    work = pendingBmp;
    pendingBmp = nullptr;
    displayState = DisplayState::Updating;
    strlcpy(stateMessage, "Actualizando la pantalla", sizeof(stateMessage));
  } else if (pendingDynamic && displayState == DisplayState::Queued) {
    pendingDynamic = false;
    dynamicLayers = pendingDynamicLayers;
    pendingDynamicLayers = 0;
    dynamic = true;
    displayState = DisplayState::Updating;
    strlcpy(stateMessage, "Componiendo la página completa", sizeof(stateMessage));
  } else if (pendingSolid >= 0 && displayState == DisplayState::Queued) {
    solid = pendingSolid;
    pendingSolid = -1;
    displayState = DisplayState::Updating;
    strlcpy(stateMessage, solid == BBEP_WHITE ? "Poniendo la pantalla en blanco"
                                             : "Poniendo la pantalla en negro",
            sizeof(stateMessage));
  }
  portEXIT_CRITICAL(&stateMux);

  if (work == nullptr && !dynamic && solid < 0) return;

  Serial.printf("Procesando actualización %lu\n", static_cast<unsigned long>(currentJob));
  const bool success = work != nullptr ? updateDisplay(work)
                                       : dynamic ? updateDynamicPage(time(nullptr), dynamicLayers ? dynamicLayers : DYNAMIC_CLOCK)
                                                 : updateSolidDisplay(solid);
  if (dynamic) calculateNextDynamicRefresh(time(nullptr));
  if (work != nullptr) heap_caps_free(work);

  if (success) {
    if (solid == BBEP_WHITE) {
      setState(DisplayState::Ready, "Prueba blanca completada");
    } else if (solid == BBEP_BLACK) {
      setState(DisplayState::Ready, "Prueba negra completada");
    } else {
      setState(DisplayState::Ready, "Pantalla actualizada");
    }
    Serial.println("Pantalla actualizada correctamente");
  } else {
    setState(DisplayState::Error, "Falló la actualización del panel");
  }
  portENTER_CRITICAL(&stateMux);
  if (pendingDynamic && displayState == DisplayState::Ready) {
    displayState = DisplayState::Queued;
    strlcpy(stateMessage, "Aplicando cambios acumulados", sizeof(stateMessage));
  }
  portEXIT_CRITICAL(&stateMux);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println("============================================================");
  Serial.println("MiEInk Studio - prueba de hardware TRMNL 7,5 pulgadas");
  Serial.println("Monitor serie: 115200 baudios");

  setupProjectStorage();
  setupDisplay();
  pinMode(AppConfig::KEY1, INPUT_PULLUP);
  pinMode(AppConfig::KEY2, INPUT_PULLUP);
  pinMode(AppConfig::KEY3, INPUT_PULLUP);
  Serial.printf("[BOOT] Botones configurables: KEY1=GPIO%u, KEY2=GPIO%u, KEY3=GPIO%u\n",
                AppConfig::KEY1, AppConfig::KEY2, AppConfig::KEY3);
  setupAccessPoint();
  setupWebServer();
  logHardwareState("ARRANQUE COMPLETO");
}

void loop() {
  dnsServer.processNextRequest();
  Connectivity::loop();
  processBackgroundData();
  pollButton(key1, "key1");
  pollButton(key2, "key2");
  pollButton(key3, "key3");
  processPendingDisplay();
  processLiveRefresh();
  const uint32_t now = millis();
  if (now - lastDiagnosticAt >= AppConfig::DIAGNOSTIC_INTERVAL_MS) {
    lastDiagnosticAt = now;
    logHardwareState("HEARTBEAT");
  }
  delay(2);
}
