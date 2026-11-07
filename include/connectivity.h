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

}  // namespace Connectivity
