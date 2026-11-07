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

namespace AppConfig {

// La configuración privada vive en include/secrets.h (ignorarlo en Git evita
// publicar la contraseña real del punto de acceso).
static constexpr char WIFI_SSID[] = MI_EINK_WIFI_SSID;
static constexpr char WIFI_PASSWORD[] = MI_EINK_WIFI_PASSWORD;
static constexpr bool DISPLAY_FLIP_180 = false;

static constexpr uint16_t DISPLAY_WIDTH = 800;
static constexpr uint16_t DISPLAY_HEIGHT = 480;
static constexpr size_t BMP_HEADER_SIZE = 62;
static constexpr size_t BMP_PIXEL_SIZE = (DISPLAY_WIDTH * DISPLAY_HEIGHT) / 8;
static constexpr size_t BMP_FILE_SIZE = BMP_HEADER_SIZE + BMP_PIXEL_SIZE;

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
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

}  // namespace AppConfig
