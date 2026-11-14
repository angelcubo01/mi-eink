#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

namespace Connectivity {

void begin();
void loop();
void registerRoutes(AsyncWebServer &server);

bool connected();
String ssid();
String ip();
int32_t rssi();
bool weatherConfigured();
bool fetchWeather(double latitude, double longitude, bool fahrenheit, int days, String &payload, String &error,
                  bool force = false);
bool fetchHolidays(int year, const String &region, String &payload, String &error);

}  // namespace Connectivity
