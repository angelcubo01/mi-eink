#pragma once

#include <Arduino.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Falta include/secrets.h. Copia include/secrets.example.h, renómbralo a secrets.h y elige una contraseña WPA2."
#endif

#ifndef MI_EINK_WIFI_SSID
#error "MI_EINK_WIFI_SSID no está definido en include/secrets.h."
#endif

#ifndef MI_EINK_WIFI_PASSWORD
#error "MI_EINK_WIFI_PASSWORD no está definido en include/secrets.h."
#endif

#ifndef MI_EINK_OPENWEATHER_API_KEY
#define MI_EINK_OPENWEATHER_API_KEY ""
#endif

#ifndef MI_EINK_HOME_WIFI_SSID
#define MI_EINK_HOME_WIFI_SSID ""
#endif

#ifndef MI_EINK_HOME_WIFI_PASSWORD
#define MI_EINK_HOME_WIFI_PASSWORD ""
#endif

namespace AppConfig {

// La configuración privada vive en include/secrets.h (ignorarlo en Git evita
// publicar la contraseña real del punto de acceso).
static constexpr char WIFI_SSID[] = MI_EINK_WIFI_SSID;
static constexpr char WIFI_PASSWORD[] = MI_EINK_WIFI_PASSWORD;
static constexpr char HOME_WIFI_SSID[] = MI_EINK_HOME_WIFI_SSID;
static constexpr char HOME_WIFI_PASSWORD[] = MI_EINK_HOME_WIFI_PASSWORD;
static constexpr char OPENWEATHER_API_KEY[] = MI_EINK_OPENWEATHER_API_KEY;
static_assert(sizeof(WIFI_SSID) > 1 && sizeof(WIFI_SSID) <= 33, "El SSID del AP debe tener entre 1 y 32 caracteres.");
static_assert(sizeof(WIFI_PASSWORD) >= 9 && sizeof(WIFI_PASSWORD) <= 64,
              "La contraseña del AP debe tener entre 8 y 63 caracteres.");
static constexpr bool DISPLAY_FLIP_180 = false;

static constexpr uint16_t DISPLAY_WIDTH = 800;
static constexpr uint16_t DISPLAY_HEIGHT = 480;
static constexpr size_t BMP_HEADER_SIZE = 62;
static constexpr size_t BMP_PIXEL_SIZE = (DISPLAY_WIDTH * DISPLAY_HEIGHT) / 8;
static constexpr size_t BMP_FILE_SIZE = BMP_HEADER_SIZE + BMP_PIXEL_SIZE;
// El proyecto Studio se guarda como JSON en LittleFS. Las imágenes cargadas
// desde móvil se reducen en el navegador antes de llegar aquí.
static constexpr size_t PROJECT_MAX_SIZE = 1024 * 1024;

// Pines oficiales del TRMNL 7,5\" OG DIY Kit con XIAO ESP32-S3 Plus.
static constexpr uint8_t EPD_SCK = 7;
static constexpr uint8_t EPD_MOSI = 9;
static constexpr uint8_t EPD_CS = 44;
static constexpr uint8_t EPD_RST = 38;
static constexpr uint8_t EPD_DC = 10;
static constexpr uint8_t EPD_BUSY = 4;
// El EE04 conmuta la alimentación de la etapa e-paper mediante D6/GPIO43.
static constexpr uint8_t EPD_ENABLE = 43;
static constexpr uint32_t EPD_SPI_SPEED = 8000000;

// Botones de usuario de la placa EE04 (activos a nivel bajo).
static constexpr uint8_t KEY1 = 2;
static constexpr uint8_t KEY2 = 3;
static constexpr uint8_t KEY3 = 5;
static constexpr uint16_t BUTTON_DEBOUNCE_MS = 40;
static constexpr uint32_t DIAGNOSTIC_INTERVAL_MS = 5000;

static constexpr uint8_t DNS_PORT = 53;
static constexpr uint32_t WIFI_CONNECTION_TIMEOUT_MS = 20000;
static constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000;
static constexpr uint32_t WIFI_AP_RECOVERY_INTERVAL_MS = 5000;
static constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

}  // namespace AppConfig
