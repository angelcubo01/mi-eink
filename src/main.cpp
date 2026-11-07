#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <SPI.h>
#include <WiFi.h>
#include <bb_epaper.h>
#include <Fonts/Roboto_Black_16.h>
#include <Fonts/Roboto_Black_24.h>
#include <Fonts/Roboto_Black_32.h>
#include <Fonts/Roboto_Black_40.h>
#include <esp_heap_caps.h>
#include <time.h>

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
  bool partial = false;
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
bool pendingPartial = false;
uint8_t partialRefreshCount = 0;
int8_t pendingSolid = -1;
bool displayReady = false;
ButtonState key1{AppConfig::KEY1};
ButtonState key2{AppConfig::KEY2};
volatile uint32_t busyTransitions = 0;
uint32_t lastDiagnosticAt = 0;
bool storageReady = false;
bool projectSaving = false;
constexpr size_t MAX_LIVE_CLOCKS = 6;
LiveClockRegion liveClocks[MAX_LIVE_CLOCKS];
size_t liveClockCount = 0;
uint16_t liveRefreshMinutes = 0;
time_t lastLiveRenderedAt = 0;
uint32_t lastLiveRefreshCheck = 0;
uint32_t liveRefreshStartedAt = 0;

constexpr char PROJECT_FILE[] = "/project.json";
constexpr char PROJECT_TEMP_FILE[] = "/project.upload";
constexpr char PROJECT_BACKUP_FILE[] = "/project.backup";
constexpr char DISPLAY_FILE[] = "/display.bmp";
constexpr char DISPLAY_TEMP_FILE[] = "/display.upload";

void loadLiveConfiguration();

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
                    pendingBmp != nullptr || pendingSolid >= 0;
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
  document["display"]["lastRefreshAt"] = nullptr;
  document["display"]["refreshCount"] = snapshot.job;
  document["display"]["refreshMinutes"] = liveRefreshMinutes;
  document["display"]["liveClockCount"] = liveClockCount;
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

void loadLiveConfiguration() {
  liveClockCount = 0;
  liveRefreshMinutes = 0;
  liveRefreshStartedAt = millis();
  lastLiveRenderedAt = 0;
  if (!storageReady || !LittleFS.exists(PROJECT_FILE)) return;

  File file = LittleFS.open(PROJECT_FILE, "r");
  if (!file) return;
  JsonDocument filter;
  filter["activePageId"] = true;
  filter["device"]["display"]["refreshMinutes"] = true;
  filter["device"]["display"]["rotation"] = true;
  filter["device"]["display"]["invert"] = true;
  filter["pages"][0]["id"] = true;
  filter["pages"][0]["widgets"][0]["type"] = true;
  filter["pages"][0]["widgets"][0]["x"] = true;
  filter["pages"][0]["widgets"][0]["y"] = true;
  filter["pages"][0]["widgets"][0]["w"] = true;
  filter["pages"][0]["widgets"][0]["h"] = true;
  filter["pages"][0]["widgets"][0]["config"] = true;
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, file, DeserializationOption::Filter(filter));
  file.close();
  if (error) {
    Serial.printf("[LIVE][WARN] No se pudo leer la configuración: %s\n", error.c_str());
    return;
  }

  liveRefreshMinutes = constrain(document["device"]["display"]["refreshMinutes"] | 0, 0, 1440);
  const int rotation = document["device"]["display"]["rotation"] | 0;
  if (rotation != 0) {
    Serial.println("[LIVE][WARN] El reloj autónomo se desactiva con rotación de 180 grados");
    return;
  }
  const bool globalInvert = document["device"]["display"]["invert"] | false;
  const String activePageId = document["activePageId"] | "";
  for (JsonObjectConst page : document["pages"].as<JsonArrayConst>()) {
    if (String(page["id"] | "") != activePageId) continue;
    for (JsonObjectConst widget : page["widgets"].as<JsonArrayConst>()) {
      if (liveClockCount >= MAX_LIVE_CLOCKS || String(widget["type"] | "") != "clock") continue;
      LiveClockRegion &clock = liveClocks[liveClockCount++];
      clock.x = constrain(widget["x"].as<int>() * 40, 0, 799);
      clock.y = constrain(widget["y"].as<int>() * 40, 0, 479);
      clock.width = constrain(widget["w"].as<int>() * 40, 1, 800 - clock.x);
      clock.height = constrain(widget["h"].as<int>() * 40, 1, 480 - clock.y);
      JsonObjectConst config = widget["config"];
      clock.showTime = config["showTime"] | true;
      clock.showWeekday = config["showWeekday"] | true;
      clock.showDate = config["showDate"] | true;
      clock.longDate = String(config["dateStyle"] | "short") == "long";
      clock.inverted = (config["invert"] | false) != globalInvert;
      clock.border = config["border"] | false;
      clock.twelveHour = String(config["format"] | "24") == "12";
    }
    break;
  }
  Serial.printf("[LIVE] Relojes autónomos=%u, intervalo=%u min\n", static_cast<unsigned>(liveClockCount),
                liveRefreshMinutes);
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
  if (context->errorCode != 0 || context->received != context->expected || !projectLooksSafeJson()) {
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
    context->partial = request->hasHeader("X-Refresh-Mode") &&
                       request->getHeader("X-Refresh-Mode")->value().equalsIgnoreCase("partial");

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
  bool acceptedPartial = false;
  portENTER_CRITICAL(&stateMux);
  if (pendingBmp == nullptr && displayState != DisplayState::Queued && displayState != DisplayState::Updating) {
    pendingBmp = context->buffer;
    pendingPartial = context->partial;
    acceptedPartial = context->partial;
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

  Serial.printf("[HTTP] BMP válido recibido: trabajo %lu, %u bytes, refresco=%s\n",
                static_cast<unsigned long>(acceptedJob), AppConfig::BMP_FILE_SIZE,
                acceptedPartial ? "PARCIAL" : "COMPLETO");
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
  const uint32_t transitionsBefore = busyTransitions;
  uint32_t startedAt = millis();
  Serial.printf("[EPD][%s] writePlane inicio: BUSY=%d, transiciones=%lu\n", source,
                digitalRead(AppConfig::EPD_BUSY), static_cast<unsigned long>(transitionsBefore));

  int result = display.writePlane();
  Serial.printf("[EPD][%s] writePlane fin: rc=%d, duración=%lums, BUSY=%d\n", source, result,
                static_cast<unsigned long>(millis() - startedAt), digitalRead(AppConfig::EPD_BUSY));
  if (result != BBEP_SUCCESS) {
    Serial.printf("[EPD][ERROR] writePlane falló con código %d\n", result);
    return false;
  }

  startedAt = millis();
  Serial.printf("[EPD][%s] refresh FULL inicio\n", source);
  result = display.refresh(REFRESH_FULL, true);
  const uint32_t elapsed = millis() - startedAt;
  const uint32_t transitionDelta = busyTransitions - transitionsBefore;
  Serial.printf("[EPD][%s] refresh fin: rc=%d, duración=%lums, BUSY=%d, cambios BUSY=%lu\n",
                source, result, static_cast<unsigned long>(elapsed), digitalRead(AppConfig::EPD_BUSY),
                static_cast<unsigned long>(transitionDelta));

  if (result != BBEP_SUCCESS) {
    Serial.printf("[EPD][ERROR] refresh falló con código %d\n", result);
    return false;
  }
  if (transitionDelta == 0) {
    Serial.println("[EPD][ERROR] BUSY no cambió durante la operación. El panel no responde: "
                   "revisa PWR_EN, puente 24Pin-GND, cable FPC y selección del controlador.");
    return false;
  }

  Serial.println("[EPD][OK] El panel respondió y completó el refresco.");
  return true;
}

uint8_t *loadPersistedDisplay() {
  if (!storageReady || !LittleFS.exists(DISPLAY_FILE)) return nullptr;
  File file = LittleFS.open(DISPLAY_FILE, "r");
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
  return Roboto_Black_16;
}

void drawLiveClock(const LiveClockRegion &clock, const struct tm &local) {
  const int background = clock.inverted ? BBEP_BLACK : BBEP_WHITE;
  const int foreground = clock.inverted ? BBEP_WHITE : BBEP_BLACK;
  display.fillRect(clock.x, clock.y, clock.width, clock.height, background);
  if (clock.border) display.drawRect(clock.x, clock.y, clock.width, clock.height, foreground);

  static const char *WEEKDAYS[] = {"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
  static const char *MONTHS[] = {"enero", "febrero", "marzo", "abril", "mayo", "junio",
                                 "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
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
      snprintf(buffer, sizeof(buffer), "%02d/%02d/%04d", local.tm_mday, local.tm_mon + 1, local.tm_year + 1900);
    }
    lines[count++] = buffer;
  }
  if (count == 0) return;

  const int padding = 8;
  const int availableWidth = max(1, clock.width - padding * 2);
  const int availableHeight = max(1, clock.height - padding * 2);
  const int sizes[] = {40, 32, 24, 16};
  int fontSize = 16;
  for (int candidate : sizes) {
    if (static_cast<int>(count) * (candidate + 4) > availableHeight) continue;
    display.setFont(clockFont(candidate));
    bool fits = true;
    for (size_t index = 0; index < count; ++index) {
      BB_RECT bounds{};
      display.getStringBox(lines[index], &bounds);
      if (bounds.w > availableWidth) fits = false;
    }
    if (fits) {
      fontSize = candidate;
      break;
    }
  }

  display.setFont(clockFont(fontSize));
  display.setTextColor(foreground, background);
  const int lineHeight = fontSize + 4;
  int baseline = clock.y + (clock.height - static_cast<int>(count) * lineHeight) / 2 + fontSize;
  for (size_t index = 0; index < count; ++index) {
    BB_RECT bounds{};
    display.getStringBox(lines[index], &bounds);
    const int x = clock.x + max(0, (clock.width - bounds.w) / 2);
    display.setCursor(x, baseline);
    display.print(lines[index]);
    baseline += lineHeight;
  }
}

bool prepareLiveFrame(const uint8_t *baseBmp, int plane, time_t timestamp) {
  display.setPlane(plane);
  display.fillScreen(BBEP_WHITE);
  int result = display.loadBMP(baseBmp, 0, 0, BBEP_WHITE, BBEP_BLACK);
  if (result != BBEP_SUCCESS) return false;
  struct tm local {};
  localtime_r(&timestamp, &local);
  for (size_t index = 0; index < liveClockCount; ++index) drawLiveClock(liveClocks[index], local);
  return true;
}

bool updateLiveClocks(time_t now) {
  uint8_t *baseBmp = loadPersistedDisplay();
  if (baseBmp == nullptr) return false;
  int result = display.allocBuffer(false);
  if (result != BBEP_SUCCESS) {
    heap_caps_free(baseBmp);
    return false;
  }

  bool success = false;
  const bool partial = lastLiveRenderedAt > 0 && partialRefreshCount < 12;
  if (partial && prepareLiveFrame(baseBmp, PLANE_1, lastLiveRenderedAt)) {
    result = display.writePlane(PLANE_1);
    if (result == BBEP_SUCCESS && prepareLiveFrame(baseBmp, PLANE_0, now)) result = display.writePlane(PLANE_0);
    if (result == BBEP_SUCCESS) result = display.refresh(REFRESH_PARTIAL, true);
    success = result == BBEP_SUCCESS;
    if (success) ++partialRefreshCount;
  }
  if (!success && prepareLiveFrame(baseBmp, PLANE_0, now)) {
    success = pushFramebuffer("RELOJ");
    if (success) partialRefreshCount = 0;
  }
  display.freeBuffer();
  heap_caps_free(baseBmp);
  if (success) lastLiveRenderedAt = now;
  return success;
}

void processLiveRefresh() {
  if (liveRefreshMinutes == 0 || liveClockCount == 0 || !displayReady || isBusy() ||
      !storageReady || !LittleFS.exists(DISPLAY_FILE)) return;
  const uint32_t nowMs = millis();
  if (nowMs - lastLiveRefreshCheck < 5000) return;
  lastLiveRefreshCheck = nowMs;
  if (nowMs - liveRefreshStartedAt < static_cast<uint32_t>(liveRefreshMinutes) * 60000UL) return;
  const time_t now = time(nullptr);
  if (now < 1700000000) return;

  portENTER_CRITICAL(&stateMux);
  displayState = DisplayState::Updating;
  strlcpy(stateMessage, "Actualizando fecha y hora", sizeof(stateMessage));
  ++currentJob;
  portEXIT_CRITICAL(&stateMux);
  Serial.println("[LIVE] Actualizando fecha/hora sin redibujar el resto del diseño");
  const bool success = updateLiveClocks(now);
  liveRefreshStartedAt = millis();
  setState(success ? DisplayState::Ready : DisplayState::Error,
           success ? "Fecha y hora actualizadas" : "Falló la actualización de fecha y hora");
}

bool pushPartialFramebuffer(const uint8_t *oldBmp, const uint8_t *newBmp) {
  const uint32_t transitionsBefore = busyTransitions;
  display.setPlane(PLANE_1);
  display.fillScreen(BBEP_WHITE);
  int result = display.loadBMP(oldBmp, 0, 0, BBEP_WHITE, BBEP_BLACK);
  if (result == BBEP_SUCCESS) result = display.writePlane(PLANE_1);
  if (result != BBEP_SUCCESS) {
    Serial.printf("[EPD][PARTIAL][ERROR] No se pudo escribir el plano anterior: %d\n", result);
    return false;
  }

  display.setPlane(PLANE_0);
  display.fillScreen(BBEP_WHITE);
  result = display.loadBMP(newBmp, 0, 0, BBEP_WHITE, BBEP_BLACK);
  if (result == BBEP_SUCCESS) result = display.writePlane(PLANE_0);
  if (result != BBEP_SUCCESS) {
    Serial.printf("[EPD][PARTIAL][ERROR] No se pudo escribir el plano nuevo: %d\n", result);
    return false;
  }

  Serial.println("[EPD][PARTIAL] Refrescando únicamente los píxeles modificados");
  result = display.refresh(REFRESH_PARTIAL, true);
  const uint32_t transitionDelta = busyTransitions - transitionsBefore;
  Serial.printf("[EPD][PARTIAL] rc=%d, cambios BUSY=%lu\n", result,
                static_cast<unsigned long>(transitionDelta));
  return result == BBEP_SUCCESS && transitionDelta > 0;
}

bool updateDisplay(const uint8_t *bmp, bool partialRequested) {
  if (!displayReady || bmp == nullptr) return false;

  Serial.println("[EPD][BMP] Reservando framebuffer...");
  int result = display.allocBuffer(false);
  if (result != BBEP_SUCCESS) {
    Serial.printf("[EPD][ERROR] No se pudo reservar el framebuffer: %d\n", result);
    return false;
  }

  uint8_t *oldBmp = partialRequested && partialRefreshCount < 12 ? loadPersistedDisplay() : nullptr;
  const bool usePartial = oldBmp != nullptr;
  display.setPlane(PLANE_0);
  display.fillScreen(BBEP_WHITE);
  // En el BMP monocromo generado por Studio, los bits a 1 usan la entrada 1
  // de la paleta (blanco) y los bits a 0 la entrada 0 (negro). loadBMP no lee
  // la paleta: su primer color corresponde directamente a los bits a 1.
  bool success = false;
  if (usePartial) {
    success = pushPartialFramebuffer(oldBmp, bmp);
    if (success) ++partialRefreshCount;
    else Serial.println("[EPD][PARTIAL][WARN] Se reintentará con refresco completo");
  }
  if (!success) {
    display.setPlane(PLANE_0);
    display.fillScreen(BBEP_WHITE);
    result = display.loadBMP(bmp, 0, 0, BBEP_WHITE, BBEP_BLACK);
    Serial.printf("[EPD][BMP] loadBMP rc=%d\n", result);
    success = result == BBEP_SUCCESS && pushFramebuffer("BMP");
    if (success) partialRefreshCount = 0;
  }
  if (oldBmp != nullptr) heap_caps_free(oldBmp);
  display.freeBuffer();

  if (!success) {
    if (result != BBEP_SUCCESS) Serial.printf("[EPD][ERROR] loadBMP falló con código %d\n", result);
    return false;
  }
  if (!persistDisplay(bmp)) Serial.println("[EPD][WARN] No se pudo persistir el último BMP");
  lastLiveRenderedAt = time(nullptr);
  liveRefreshStartedAt = millis();
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

void pollButton(ButtonState &button, int color, const char *message) {
  const bool reading = digitalRead(button.pin);
  const uint32_t now = millis();

  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.changedAt = now;
  }

  if (reading != button.stableState && now - button.changedAt >= AppConfig::BUTTON_DEBOUNCE_MS) {
    button.stableState = reading;
    if (reading == LOW) {
      if (queueSolidDisplay(color, message)) {
        Serial.printf("Botón GPIO%u: %s\n", button.pin, message);
      } else {
        Serial.printf("Botón GPIO%u ignorado: pantalla ocupada\n", button.pin);
      }
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
  server.on("/api/display", HTTP_POST, finishDisplayUpload, nullptr, handleDisplayBody);
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
  loadLiveConfiguration();
}

void setupAccessPoint() {
  Connectivity::begin();
  dnsServer.start(AppConfig::DNS_PORT, "*", AppConfig::AP_IP);
}

void processPendingDisplay() {
  uint8_t *work = nullptr;
  bool partial = false;
  int solid = -1;

  portENTER_CRITICAL(&stateMux);
  if (pendingBmp != nullptr && displayState == DisplayState::Queued) {
    work = pendingBmp;
    pendingBmp = nullptr;
    partial = pendingPartial;
    pendingPartial = false;
    displayState = DisplayState::Updating;
    strlcpy(stateMessage, "Actualizando la pantalla", sizeof(stateMessage));
  } else if (pendingSolid >= 0 && displayState == DisplayState::Queued) {
    solid = pendingSolid;
    pendingSolid = -1;
    displayState = DisplayState::Updating;
    strlcpy(stateMessage, solid == BBEP_WHITE ? "Poniendo la pantalla en blanco"
                                             : "Poniendo la pantalla en negro",
            sizeof(stateMessage));
  }
  portEXIT_CRITICAL(&stateMux);

  if (work == nullptr && solid < 0) return;

  Serial.printf("Procesando actualización %lu\n", static_cast<unsigned long>(currentJob));
  const bool success = work != nullptr ? updateDisplay(work, partial) : updateSolidDisplay(solid);
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
  Serial.printf("[BOOT] Botones: KEY1=GPIO%u (blanco), KEY2=GPIO%u (negro), KEY3=GPIO%u\n",
                AppConfig::KEY1, AppConfig::KEY2, AppConfig::KEY3);
  setupAccessPoint();
  setupWebServer();
  logHardwareState("ARRANQUE COMPLETO");
}

void loop() {
  dnsServer.processNextRequest();
  Connectivity::loop();
  pollButton(key1, BBEP_WHITE, "Prueba blanca en cola");
  pollButton(key2, BBEP_BLACK, "Prueba negra en cola");
  processPendingDisplay();
  processLiveRefresh();
  const uint32_t now = millis();
  if (now - lastDiagnosticAt >= AppConfig::DIAGNOSTIC_INTERVAL_MS) {
    lastDiagnosticAt = now;
    logHardwareState("HEARTBEAT");
  }
  delay(2);
}
