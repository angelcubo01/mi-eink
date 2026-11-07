#include "connectivity.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "app_config.h"

namespace Connectivity {
namespace {

Preferences preferences;
String configuredSsid;
String configuredPassword;
bool connectPending = false;
uint32_t connectRequestedAt = 0;
uint32_t lastReconnectAt = 0;
uint32_t connectionStartedAt = 0;
uint32_t lastApRecoveryAt = 0;
bool accessPointStarted = false;
uint8_t lastDisconnectReason = 0;

enum class NetworkState : uint8_t { NotConfigured, Connecting, Connected, Failed };
NetworkState networkState = NetworkState::NotConfigured;

struct WeatherCache {
  String key;
  String payload;
  uint32_t storedAt = 0;
};

WeatherCache weatherCache;
constexpr char HOLIDAY_CACHE_FILE[] = "/holidays.json";
constexpr char HOLIDAY_CACHE_TEMP_FILE[] = "/holidays.upload";

struct NetworkRequestContext {
  String body;
  bool tooLarge = false;
};

void sendJson(AsyncWebServerRequest *request, int status, const JsonDocument &document) {
  String payload;
  serializeJson(document, payload);
  AsyncWebServerResponse *response = request->beginResponse(status, "application/json; charset=utf-8", payload);
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("X-Content-Type-Options", "nosniff");
  request->send(response);
}

void sendError(AsyncWebServerRequest *request, int status, const String &message) {
  JsonDocument document;
  document["message"] = message;
  sendJson(request, status, document);
}

String urlEncode(const String &value) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t index = 0; index < value.length(); ++index) {
    const uint8_t character = static_cast<uint8_t>(value[index]);
    if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.' ||
        character == '~') {
      encoded += static_cast<char>(character);
    } else {
      encoded += '%';
      encoded += HEX_DIGITS[character >> 4U];
      encoded += HEX_DIGITS[character & 0x0fU];
    }
  }
  return encoded;
}

String isoTime(int64_t unixSeconds, int32_t offsetSeconds = 0, bool localWithoutZone = false) {
  if (unixSeconds <= 0) return String();
  const time_t adjusted = static_cast<time_t>(unixSeconds + offsetSeconds);
  struct tm parts {};
  gmtime_r(&adjusted, &parts);
  char buffer[32];
  strftime(buffer, sizeof(buffer), localWithoutZone ? "%Y-%m-%dT%H:%M:%S" : "%Y-%m-%dT%H:%M:%SZ", &parts);
  return String(buffer);
}

String encryptionName(wifi_auth_mode_t encryption) {
  switch (encryption) {
    case WIFI_AUTH_OPEN:
      return "open";
    case WIFI_AUTH_WEP:
      return "wep";
    case WIFI_AUTH_WPA_PSK:
      return "wpa";
    case WIFI_AUTH_WPA2_PSK:
      return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "wpa_wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "enterprise";
    case WIFI_AUTH_WPA3_PSK:
      return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "wpa2_wpa3";
    default:
      return "secured";
  }
}

const char *disconnectReasonMessage(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
      return "No se encuentra esa red. Comprueba que sea Wi-Fi de 2,4 GHz y que esté al alcance.";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "No se pudo autenticar. Revisa la contraseña de la red doméstica.";
    case WIFI_REASON_ASSOC_FAIL:
      return "El router rechazó la asociación del dispositivo.";
    default:
      return "Se perdió la conexión con la red doméstica.";
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      accessPointStarted = true;
      Serial.printf("[WIFI][AP] Disponible en http://%s\n", WiFi.softAPIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      accessPointStarted = false;
      Serial.println("[WIFI][AP][WARN] El AP se detuvo; se recuperará automáticamente");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      networkState = NetworkState::Connecting;
      Serial.println("[WIFI][STA] Asociado al router; esperando dirección IP...");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      networkState = NetworkState::Connected;
      lastDisconnectReason = 0;
      // Zona peninsular española con cambio automático CET/CEST. El reloj del
      // controlador usa NTP para poder mantener fecha y hora sin el navegador.
      configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.google.com");
      Serial.printf("[WIFI][STA][OK] Internet por '%s', IP=%s, RSSI=%ld dBm\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), static_cast<long>(WiFi.RSSI()));
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (!configuredSsid.isEmpty()) {
        networkState = NetworkState::Failed;
        lastDisconnectReason = info.wifi_sta_disconnected.reason;
        Serial.printf("[WIFI][STA][WARN] Desconectado, motivo=%u: %s\n", lastDisconnectReason,
                      disconnectReasonMessage(lastDisconnectReason));
      }
      break;
    default:
      break;
  }
}

bool startAccessPoint() {
  lastApRecoveryAt = millis();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AppConfig::AP_IP, AppConfig::AP_GATEWAY, AppConfig::AP_SUBNET);
  accessPointStarted = WiFi.softAP(AppConfig::WIFI_SSID, AppConfig::WIFI_PASSWORD, 1, false, 4);
  if (!accessPointStarted) Serial.println("[WIFI][AP][ERROR] No se pudo iniciar el punto de acceso");
  return accessPointStarted;
}

void startStationConnection() {
  if (configuredSsid.isEmpty()) {
    networkState = NetworkState::NotConfigured;
    WiFi.disconnect(false, false);
    Serial.println("[WIFI] No hay red doméstica configurada; el AP sigue disponible");
    return;
  }
  networkState = NetworkState::Connecting;
  lastDisconnectReason = 0;
  connectionStartedAt = millis();
  Serial.printf("[WIFI] Conectando a la red doméstica '%s'...\n", configuredSsid.c_str());
  WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
  lastReconnectAt = millis();
}

void loadCredentials() {
  preferences.begin("mieink", false);
  configuredSsid = preferences.getString("home_ssid", AppConfig::HOME_WIFI_SSID);
  configuredPassword = preferences.getString("home_pass", AppConfig::HOME_WIFI_PASSWORD);
}

void saveCredentials(const String &network, const String &password) {
  configuredSsid = network;
  configuredPassword = password;
  preferences.putString("home_ssid", configuredSsid);
  preferences.putString("home_pass", configuredPassword);
  connectPending = true;
  connectRequestedAt = millis();
  networkState = NetworkState::Connecting;
  lastDisconnectReason = 0;
}

void handleNetworkStatus(AsyncWebServerRequest *request) {
  JsonDocument document;
  document["mode"] = "ap_sta";
  document["apSsid"] = AppConfig::WIFI_SSID;
  document["homeSsid"] = configuredSsid;
  document["configured"] = !configuredSsid.isEmpty();
  document["connected"] = connected();
  const char *state = "error";
  if (connected()) state = "connected";
  else if (configuredSsid.isEmpty()) state = "not_configured";
  else if (networkState == NetworkState::Connecting || connectPending) state = "connecting";
  document["state"] = state;
  document["apActive"] = accessPointStarted;
  document["apIp"] = WiFi.softAPIP().toString();
  document["ip"] = connected() ? WiFi.localIP().toString() : String();
  if (connected()) document["rssi"] = WiFi.RSSI();
  else document["rssi"] = nullptr;
  if (!connected() && networkState == NetworkState::Failed) {
    document["error"] = disconnectReasonMessage(lastDisconnectReason);
    document["disconnectReason"] = lastDisconnectReason;
  } else {
    document["error"] = nullptr;
    document["disconnectReason"] = nullptr;
  }
  document["weatherConfigured"] = weatherConfigured();
  sendJson(request, 200, document);
}

void handleNetworkScan(AsyncWebServerRequest *request) {
  const int count = WiFi.scanNetworks(false, true, false, 350);
  if (count < 0) {
    sendError(request, 503, "No se pudo escanear el entorno Wi-Fi. Vuelve a intentarlo.");
    return;
  }

  struct FoundNetwork {
    String ssid;
    int32_t rssi;
    wifi_auth_mode_t encryption;
  };
  std::vector<FoundNetwork> networks;
  networks.reserve(count);
  for (int index = 0; index < count; ++index) {
    const String foundSsid = WiFi.SSID(index);
    if (foundSsid.isEmpty()) continue;
    auto existing = std::find_if(networks.begin(), networks.end(), [&](const FoundNetwork &item) {
      return item.ssid == foundSsid;
    });
    if (existing == networks.end()) {
      networks.push_back({foundSsid, WiFi.RSSI(index), WiFi.encryptionType(index)});
    } else if (WiFi.RSSI(index) > existing->rssi) {
      existing->rssi = WiFi.RSSI(index);
      existing->encryption = WiFi.encryptionType(index);
    }
  }
  WiFi.scanDelete();
  std::sort(networks.begin(), networks.end(), [](const FoundNetwork &left, const FoundNetwork &right) {
    return left.rssi > right.rssi;
  });

  JsonDocument document;
  JsonArray result = document["networks"].to<JsonArray>();
  for (const FoundNetwork &network : networks) {
    JsonObject item = result.add<JsonObject>();
    item["ssid"] = network.ssid;
    item["rssi"] = network.rssi;
    item["security"] = encryptionName(network.encryption);
    item["open"] = network.encryption == WIFI_AUTH_OPEN;
    item["configured"] = network.ssid == configuredSsid;
  }
  document["connectedSsid"] = connected() ? WiFi.SSID() : String();
  sendJson(request, 200, document);
}

void handleNetworkBody(AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t index, size_t total) {
  auto *context = static_cast<NetworkRequestContext *>(request->_tempObject);
  if (index == 0) {
    context = new (std::nothrow) NetworkRequestContext();
    request->_tempObject = context;
    if (context == nullptr) return;
    request->onDisconnect([request]() {
      auto *orphan = static_cast<NetworkRequestContext *>(request->_tempObject);
      request->_tempObject = nullptr;
      delete orphan;
    });
    context->tooLarge = total == 0 || total > 2048;
    if (!context->tooLarge) context->body.reserve(total);
  }
  context = static_cast<NetworkRequestContext *>(request->_tempObject);
  if (context == nullptr || context->tooLarge || index + length > 2048) return;
  context->body.concat(reinterpret_cast<const char *>(data), length);
}

void finishNetworkSave(AsyncWebServerRequest *request) {
  auto *context = static_cast<NetworkRequestContext *>(request->_tempObject);
  request->_tempObject = nullptr;
  if (context == nullptr) {
    sendError(request, 500, "No se pudo recibir la configuración de red.");
    return;
  }
  if (context->tooLarge) {
    delete context;
    sendError(request, 413, "La configuración de red es demasiado grande.");
    return;
  }
  JsonDocument document;
  const DeserializationError jsonError = deserializeJson(document, context->body);
  delete context;
  if (jsonError || !document.is<JsonObject>()) {
    sendError(request, 400, "La configuración de red no es JSON válido.");
    return;
  }
  String network = document["homeSsid"] | "";
  String password = document["homePassword"] | "";
  network.trim();
  if (network.isEmpty() || network.length() > 32) {
    sendError(request, 400, "Selecciona o escribe una red Wi-Fi válida.");
    return;
  }
  if (password.isEmpty() && network == configuredSsid) password = configuredPassword;
  if (!password.isEmpty() && (password.length() < 8 || password.length() > 63)) {
    sendError(request, 400, "La contraseña Wi-Fi debe tener entre 8 y 63 caracteres.");
    return;
  }
  saveCredentials(network, password);
  JsonDocument response;
  response["message"] = "Red guardada. El controlador intentará conectarse ahora.";
  response["network"]["mode"] = "ap_sta";
  response["network"]["apSsid"] = AppConfig::WIFI_SSID;
  response["network"]["homeSsid"] = configuredSsid;
  response["network"]["configured"] = true;
  response["secretsAccepted"] = true;
  sendJson(request, 202, response);
}

bool externalJson(const String &url, JsonDocument &document, String &error) {
  if (!connected()) {
    error = "El controlador no está conectado a la red doméstica.";
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(AppConfig::HTTP_TIMEOUT_MS);
  http.setTimeout(AppConfig::HTTP_TIMEOUT_MS);
  // Fuerza una respuesta finita: algunos proveedores mantienen la conexión
  // abierta y pueden bloquear temporalmente la interfaz web del controlador.
  http.useHTTP10(true);
  http.setReuse(false);
  http.setUserAgent("MiEInk-Studio/1.0");
  if (!http.begin(client, url)) {
    error = "No se pudo iniciar la conexión segura con el servicio externo.";
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close");
  const int status = http.GET();
  if (status < 200 || status >= 300) {
    error = status > 0 ? "El servicio externo respondió " + String(status) + "." : "Falló la conexión con el servicio externo.";
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();
  const DeserializationError jsonError = deserializeJson(document, payload);
  if (jsonError) {
    error = "El servicio externo devolvió una respuesta no válida.";
    return false;
  }
  return true;
}

bool readHolidayCache(int year, const String &region, JsonDocument &document) {
  File file = LittleFS.open(HOLIDAY_CACHE_FILE, "r");
  if (!file) return false;
  const DeserializationError error = deserializeJson(document, file);
  file.close();
  return !error && document["year"].as<int>() == year && String(document["region"] | "") == region &&
         document["holidays"].is<JsonArray>();
}

bool writeHolidayCache(const JsonDocument &document) {
  LittleFS.remove(HOLIDAY_CACHE_TEMP_FILE);
  File file = LittleFS.open(HOLIDAY_CACHE_TEMP_FILE, "w");
  if (!file) return false;
  const size_t written = serializeJson(document, file);
  file.flush();
  file.close();
  if (written == 0) {
    LittleFS.remove(HOLIDAY_CACHE_TEMP_FILE);
    return false;
  }
  LittleFS.remove(HOLIDAY_CACHE_FILE);
  if (!LittleFS.rename(HOLIDAY_CACHE_TEMP_FILE, HOLIDAY_CACHE_FILE)) {
    LittleFS.remove(HOLIDAY_CACHE_TEMP_FILE);
    return false;
  }
  return true;
}

double precipitation(JsonVariantConst entry) {
  return entry["rain"]["1h"].as<double>() + entry["rain"]["3h"].as<double>() +
         entry["snow"]["1h"].as<double>() + entry["snow"]["3h"].as<double>();
}

double windSpeed(double value, bool fahrenheit) {
  return fahrenheit ? value : value * 3.6;
}

void handleGeocode(AsyncWebServerRequest *request) {
  if (!weatherConfigured()) {
    sendError(request, 503, "Falta MI_EINK_OPENWEATHER_API_KEY en include/secrets.h.");
    return;
  }
  if (!request->hasParam("q") || request->getParam("q")->value().length() < 2) {
    sendError(request, 400, "Escribe al menos dos caracteres.");
    return;
  }
  const String query = request->getParam("q")->value();
  const String url = "https://api.openweathermap.org/geo/1.0/direct?q=" + urlEncode(query) +
                     "&limit=5&appid=" + urlEncode(AppConfig::OPENWEATHER_API_KEY);
  JsonDocument upstream;
  String error;
  if (!externalJson(url, upstream, error)) {
    sendError(request, 502, error);
    return;
  }
  JsonDocument response;
  JsonArray results = response["results"].to<JsonArray>();
  for (JsonObjectConst place : upstream.as<JsonArrayConst>()) {
    JsonObject result = results.add<JsonObject>();
    result["name"] = place["local_names"]["es"] | place["name"] | "";
    result["state"] = place["state"] | "";
    result["country"] = place["country"] | "";
    result["latitude"] = place["lat"];
    result["longitude"] = place["lon"];
  }
  sendJson(request, 200, response);
}

struct DailyWeather {
  String date;
  String condition;
  int code = 0;
  double minimum = INFINITY;
  double maximum = -INFINITY;
  double apparentMinimum = INFINITY;
  double apparentMaximum = -INFINITY;
  double precipitation = 0;
  int precipitationProbability = 0;
  double wind = 0;
  int representativeDistance = 99;
  String sunrise;
  String sunset;
};

DailyWeather *dayFor(std::vector<DailyWeather> &days, const String &date) {
  for (DailyWeather &day : days) if (day.date == date) return &day;
  days.push_back(DailyWeather{});
  days.back().date = date;
  return &days.back();
}

void addWeatherEntry(std::vector<DailyWeather> &days, JsonVariantConst entry, int32_t offset, bool fahrenheit,
                     bool currentEntry) {
  const int64_t timestamp = entry["dt"].as<int64_t>();
  const String local = isoTime(timestamp, offset, true);
  if (local.length() < 13) return;
  DailyWeather *day = dayFor(days, local.substring(0, 10));
  const double temperature = entry["main"]["temp"].as<double>();
  const double minimum = entry["main"]["temp_min"].is<double>() ? entry["main"]["temp_min"].as<double>() : temperature;
  const double maximum = entry["main"]["temp_max"].is<double>() ? entry["main"]["temp_max"].as<double>() : temperature;
  const double apparent = entry["main"]["feels_like"].is<double>() ? entry["main"]["feels_like"].as<double>() : temperature;
  day->minimum = std::min(day->minimum, minimum);
  day->maximum = std::max(day->maximum, maximum);
  day->apparentMinimum = std::min(day->apparentMinimum, apparent);
  day->apparentMaximum = std::max(day->apparentMaximum, apparent);
  day->precipitation += precipitation(entry);
  day->precipitationProbability = std::max(day->precipitationProbability,
                                           static_cast<int>(round(entry["pop"].as<double>() * 100.0)));
  day->wind = std::max(day->wind, windSpeed(entry["wind"]["speed"].as<double>(), fahrenheit));
  const int hour = local.substring(11, 13).toInt();
  const int distance = currentEntry ? -1 : abs(hour - 12);
  if (distance < day->representativeDistance) {
    day->representativeDistance = distance;
    day->code = entry["weather"][0]["id"] | 0;
    day->condition = entry["weather"][0]["description"] | "";
  }
}

void handleWeather(AsyncWebServerRequest *request) {
  if (!weatherConfigured()) {
    sendError(request, 503, "Falta MI_EINK_OPENWEATHER_API_KEY en include/secrets.h.");
    return;
  }
  if (!request->hasParam("latitude") || !request->hasParam("longitude")) {
    sendError(request, 400, "Coordenadas no válidas.");
    return;
  }
  const double latitude = request->getParam("latitude")->value().toDouble();
  const double longitude = request->getParam("longitude")->value().toDouble();
  if (latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
    sendError(request, 400, "Coordenadas no válidas.");
    return;
  }
  const bool fahrenheit = request->hasParam("units") && request->getParam("units")->value() == "fahrenheit";
  const int daysRequested = constrain(request->hasParam("days") ? request->getParam("days")->value().toInt() : 3, 1, 5);
  const String cacheKey = String(latitude, 4) + ":" + String(longitude, 4) + ":" + (fahrenheit ? "f" : "c") + ":" + daysRequested;
  if (weatherCache.key == cacheKey && !weatherCache.payload.isEmpty() &&
      millis() - weatherCache.storedAt < 60UL * 60UL * 1000UL) {
    JsonDocument cached;
    if (!deserializeJson(cached, weatherCache.payload)) {
      cached["cached"] = true;
      sendJson(request, 200, cached);
      return;
    }
  }

  const String common = "lat=" + String(latitude, 6) + "&lon=" + String(longitude, 6) +
                        "&appid=" + urlEncode(AppConfig::OPENWEATHER_API_KEY) +
                        "&units=" + (fahrenheit ? "imperial" : "metric") + "&lang=es";
  JsonDocument current;
  JsonDocument forecast;
  String error;
  if (!externalJson("https://api.openweathermap.org/data/2.5/weather?" + common, current, error) ||
      !externalJson("https://api.openweathermap.org/data/2.5/forecast?" + common, forecast, error)) {
    sendError(request, 502, error);
    return;
  }

  const int32_t offset = forecast["city"]["timezone"] | current["timezone"] | 0;
  std::vector<DailyWeather> daily;
  daily.reserve(6);
  addWeatherEntry(daily, current.as<JsonVariantConst>(), offset, fahrenheit, true);
  for (JsonVariantConst entry : forecast["list"].as<JsonArrayConst>()) {
    addWeatherEntry(daily, entry, offset, fahrenheit, false);
  }
  std::sort(daily.begin(), daily.end(), [](const DailyWeather &left, const DailyWeather &right) {
    return left.date < right.date;
  });
  const String currentDate = isoTime(current["dt"].as<int64_t>(), offset, true).substring(0, 10);
  for (DailyWeather &day : daily) {
    if (day.date == currentDate) {
      day.sunrise = isoTime(current["sys"]["sunrise"].as<int64_t>(), offset, true);
      day.sunset = isoTime(current["sys"]["sunset"].as<int64_t>(), offset, true);
    }
  }

  JsonDocument response;
  response["provider"] = "OpenWeather";
  response["fetchedAt"] = isoTime(current["dt"].as<int64_t>());
  response["units"]["temperature"] = fahrenheit ? "°F" : "°C";
  response["units"]["wind"] = fahrenheit ? "mph" : "km/h";
  response["units"]["precipitation"] = fahrenheit ? "in" : "mm";
  response["temperature"] = current["main"]["temp"];
  response["apparentTemperature"] = current["main"]["feels_like"];
  response["humidity"] = current["main"]["humidity"];
  response["pressure"] = current["main"]["pressure"];
  response["precipitation"] = round(precipitation(current.as<JsonVariantConst>()) * 100.0) / 100.0;
  response["cloudCover"] = current["clouds"]["all"];
  response["visibility"] = current["visibility"].as<double>() / 1000.0;
  response["code"] = current["weather"][0]["id"];
  response["condition"] = current["weather"][0]["description"] | "";
  response["wind"] = round(windSpeed(current["wind"]["speed"].as<double>(), fahrenheit) * 10.0) / 10.0;
  response["windDirection"] = current["wind"]["deg"];
  if (!current["wind"]["gust"].isNull()) {
    response["windGust"] = round(windSpeed(current["wind"]["gust"].as<double>(), fahrenheit) * 10.0) / 10.0;
  }
  const int64_t now = current["dt"].as<int64_t>();
  response["isDay"] = now >= current["sys"]["sunrise"].as<int64_t>() && now < current["sys"]["sunset"].as<int64_t>();
  JsonArray dailyJson = response["daily"].to<JsonArray>();
  for (size_t index = 0; index < daily.size() && static_cast<int>(index) < daysRequested; ++index) {
    const DailyWeather &day = daily[index];
    JsonObject item = dailyJson.add<JsonObject>();
    item["date"] = day.date;
    item["code"] = day.code;
    item["condition"] = day.condition;
    item["min"] = std::isfinite(day.minimum) ? day.minimum : 0;
    item["max"] = std::isfinite(day.maximum) ? day.maximum : 0;
    item["apparentMin"] = std::isfinite(day.apparentMinimum) ? day.apparentMinimum : 0;
    item["apparentMax"] = std::isfinite(day.apparentMaximum) ? day.apparentMaximum : 0;
    item["precipitation"] = round(day.precipitation * 100.0) / 100.0;
    item["precipitationProbability"] = day.precipitationProbability;
    item["wind"] = round(day.wind * 10.0) / 10.0;
    if (!day.sunrise.isEmpty()) item["sunrise"] = day.sunrise;
    if (!day.sunset.isEmpty()) item["sunset"] = day.sunset;
  }
  response["precipitationProbability"] = daily.empty() ? 0 : daily.front().precipitationProbability;
  response["cached"] = false;
  serializeJson(response, weatherCache.payload);
  weatherCache.key = cacheKey;
  weatherCache.storedAt = millis();
  sendJson(request, 200, response);
}

void handleHolidays(AsyncWebServerRequest *request) {
  const int year = request->hasParam("year") ? request->getParam("year")->value().toInt() : 0;
  const String region = request->hasParam("region") ? request->getParam("region")->value() : String();
  if (year < 2000 || year > 2100) {
    sendError(request, 400, "Año no válido.");
    return;
  }
  if (!region.isEmpty() && (region.length() != 5 || !region.startsWith("ES-"))) {
    sendError(request, 400, "Comunidad autónoma no válida.");
    return;
  }
  JsonDocument cached;
  const bool hasCached = readHolidayCache(year, region, cached);
  const bool force = request->hasParam("force") && request->getParam("force")->value() == "1";
  if (hasCached && !force) {
    cached["cached"] = true;
    sendJson(request, 200, cached);
    return;
  }
  JsonDocument upstream;
  String error;
  // Este endpoint está respaldado por CDN y es más estable para el ESP32 que
  // el host v4, que a veces agota el tiempo de espera.
  if (!externalJson("https://date.nager.at/api/v3/PublicHolidays/" + String(year) + "/ES", upstream, error)) {
    if (hasCached) {
      cached["cached"] = true;
      cached["stale"] = true;
      sendJson(request, 200, cached);
      return;
    }
    sendError(request, 502, error);
    return;
  }
  JsonDocument response;
  response["year"] = year;
  response["region"] = region;
  response["cached"] = false;
  JsonArray holidays = response["holidays"].to<JsonArray>();
  for (JsonObjectConst holiday : upstream.as<JsonArrayConst>()) {
    const bool global = holiday["global"] | holiday["nationalHoliday"] | false;
    bool regional = false;
    JsonArrayConst subdivisions = holiday["counties"].is<JsonArrayConst>()
                                      ? holiday["counties"].as<JsonArrayConst>()
                                      : holiday["subdivisionCodes"].as<JsonArrayConst>();
    for (const char *subdivision : subdivisions) {
      if (region == subdivision) regional = true;
    }
    if (!global && (!regional || region.isEmpty())) continue;
    const String date = holiday["date"] | "";
    const String title = holiday["localName"] | holiday["name"] | "Festivo";
    JsonObject item = holidays.add<JsonObject>();
    item["id"] = "es-" + date + "-" + String(holidays.size());
    item["date"] = date;
    item["title"] = title;
    item["scope"] = global ? "Nacional" : region;
  }
  if (!writeHolidayCache(response)) Serial.println("[HOLIDAYS][WARN] No se pudo guardar la caché en LittleFS");
  sendJson(request, 200, response);
}

}  // namespace

void begin() {
  loadCredentials();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  if (startAccessPoint()) {
    Serial.printf("[WIFI] AP '%s' listo en http://%s\n", AppConfig::WIFI_SSID, WiFi.softAPIP().toString().c_str());
  }
  startStationConnection();
}

void loop() {
  if (!accessPointStarted && millis() - lastApRecoveryAt >= AppConfig::WIFI_AP_RECOVERY_INTERVAL_MS) {
    Serial.println("[WIFI][AP] Intentando recuperar el punto de acceso...");
    startAccessPoint();
  }
  if (connectPending && millis() - connectRequestedAt >= 500) {
    connectPending = false;
    WiFi.disconnect(false, false);
    startStationConnection();
  }
  if (networkState == NetworkState::Connecting && !connected() &&
      millis() - connectionStartedAt >= AppConfig::WIFI_CONNECTION_TIMEOUT_MS) {
    networkState = NetworkState::Failed;
    Serial.println("[WIFI][STA][WARN] Tiempo de conexión agotado; el AP de configuración sigue activo");
  }
  if (!configuredSsid.isEmpty() && !connected() && !connectPending &&
      millis() - lastReconnectAt >= AppConfig::WIFI_RECONNECT_INTERVAL_MS) {
    startStationConnection();
  }
}

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/device/network", HTTP_GET, handleNetworkStatus);
  server.on("/api/device/networks", HTTP_GET, handleNetworkScan);
  server.on("/api/device/network", HTTP_POST, finishNetworkSave, nullptr, handleNetworkBody);

  server.on("/api/integrations/geocode", HTTP_GET, handleGeocode);
  server.on("/api/integrations/weather", HTTP_GET, handleWeather);
  server.on("/api/integrations/holidays", HTTP_GET, handleHolidays);
}

bool connected() {
  return WiFi.status() == WL_CONNECTED;
}

String ssid() {
  return connected() ? WiFi.SSID() : (configuredSsid.isEmpty() ? String(AppConfig::WIFI_SSID) : configuredSsid);
}

String ip() {
  return connected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
}

int32_t rssi() {
  return connected() ? WiFi.RSSI() : 0;
}

bool weatherConfigured() {
  return AppConfig::OPENWEATHER_API_KEY[0] != '\0';
}

}  // namespace Connectivity
