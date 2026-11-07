#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <WiFi.h>
#include <bb_epaper.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <new>

#include "app_config.h"
#include "web_assets.h"

namespace {

enum class DisplayState : uint8_t { Ready, Queued, Updating, Error };

struct UploadContext {
  uint8_t *buffer = nullptr;
  size_t received = 0;
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

AsyncWebServer server(80);
DNSServer dnsServer;
BBEPAPER display;

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
DisplayState displayState = DisplayState::Ready;
uint32_t currentJob = 0;
char stateMessage[96] = "Preparado";
uint8_t *pendingBmp = nullptr;
int8_t pendingSolid = -1;
bool displayReady = false;
ButtonState key1{AppConfig::KEY1};
ButtonState key2{AppConfig::KEY2};
volatile uint32_t busyTransitions = 0;
uint32_t lastDiagnosticAt = 0;

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

  Serial.printf("[HTTP] BMP válido recibido: trabajo %lu, %u bytes\n",
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

bool updateDisplay(const uint8_t *bmp) {
  if (!displayReady || bmp == nullptr) return false;

  Serial.println("[EPD][BMP] Reservando framebuffer...");
  int result = display.allocBuffer(false);
  if (result != BBEP_SUCCESS) {
    Serial.printf("[EPD][ERROR] No se pudo reservar el framebuffer: %d\n", result);
    return false;
  }

  display.fillScreen(BBEP_WHITE);
  result = display.loadBMP(bmp, 0, 0, BBEP_BLACK, BBEP_WHITE);
  Serial.printf("[EPD][BMP] loadBMP rc=%d\n", result);
  const bool success = result == BBEP_SUCCESS && pushFramebuffer("BMP");
  display.freeBuffer();

  if (!success) {
    if (result != BBEP_SUCCESS) Serial.printf("[EPD][ERROR] loadBMP falló con código %d\n", result);
    return false;
  }
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
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/display", HTTP_POST, finishDisplayUpload, nullptr, handleDisplayBody);

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

void setupAccessPoint() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AppConfig::AP_IP, AppConfig::AP_GATEWAY, AppConfig::AP_SUBNET);
  const bool started = WiFi.softAP(AppConfig::WIFI_SSID, AppConfig::WIFI_PASSWORD, 1, false, 4);
  if (!started) {
    Serial.println("No se pudo iniciar el punto de acceso Wi-Fi");
    setState(DisplayState::Error, "No se pudo iniciar la red Wi-Fi");
    return;
  }

  dnsServer.start(AppConfig::DNS_PORT, "*", AppConfig::AP_IP);
  Serial.printf("Red Wi-Fi: %s\n", AppConfig::WIFI_SSID);
  Serial.printf("Web: http://%s\n", WiFi.softAPIP().toString().c_str());
}

void processPendingDisplay() {
  uint8_t *work = nullptr;
  int solid = -1;

  portENTER_CRITICAL(&stateMux);
  if (pendingBmp != nullptr && displayState == DisplayState::Queued) {
    work = pendingBmp;
    pendingBmp = nullptr;
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
  const bool success = work != nullptr ? updateDisplay(work) : updateSolidDisplay(solid);
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
  Serial.println("EINK Texto - firmware de diagnóstico TRMNL 7,5 pulgadas");
  Serial.println("Monitor serie: 115200 baudios");

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
  pollButton(key1, BBEP_WHITE, "Prueba blanca en cola");
  pollButton(key2, BBEP_BLACK, "Prueba negra en cola");
  processPendingDisplay();
  const uint32_t now = millis();
  if (now - lastDiagnosticAt >= AppConfig::DIAGNOSTIC_INTERVAL_MS) {
    lastDiagnosticAt = now;
    logHardwareState("HEARTBEAT");
  }
  delay(2);
}
