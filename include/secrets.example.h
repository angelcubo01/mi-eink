#pragma once

// Copia este archivo como include/secrets.h y personaliza estos valores.
// secrets.h está ignorado por Git y nunca debe subirse a un repositorio público.
// La contraseña WPA2 necesita ocho caracteres o más.
#define MI_EINK_WIFI_SSID "MiEInk"
#define MI_EINK_WIFI_PASSWORD "cambia-esta-clave"

// Opcional: primera conexión doméstica. Después puede elegirse y cambiarse
// desde Dispositivo > Red sin volver a compilar. Se guarda en la NVS del ESP32.
#define MI_EINK_HOME_WIFI_SSID ""
#define MI_EINK_HOME_WIFI_PASSWORD ""

// Clave privada usada por el controlador para geocodificar y consultar el tiempo.
// Nunca se entrega al navegador ni se guarda dentro del proyecto Studio.
#define MI_EINK_OPENWEATHER_API_KEY ""
