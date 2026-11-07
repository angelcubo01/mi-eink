# MiEInk

Estudio visual local para diseñar paneles multipágina de 800 × 480 píxeles y enviarlos a un **TRMNL 7,5" (OG) DIY Kit** monocromo. No usa TRMNL Cloud ni requiere cuentas.

La interfaz se puede desarrollar completamente en el ordenador mediante una simulación local; solo necesitas flashear el XIAO cuando quieras probar el panel e-paper real.

> Cargar el firmware sustituye el firmware TRMNL instalado de fábrica. Puedes restaurarlo con el flasheador oficial de TRMNL.

## Hardware compatible

- TRMNL 7,5" (OG) DIY Kit monocromo de 800 × 480.
- XIAO ESP32-S3 Plus integrado en la placa EE04.
- Panel UC8179 conectado al conector de 24 pines.

No es compatible con la variante XIAO ESP32-C3 ni con los modelos de tres o seis colores.

## Desarrollo de la interfaz sin hardware

La simulación incluye el editor MiEInk Studio: páginas, grid 20 × 12, widgets de texto, reloj, tiempo, calendario, eventos, estado del dispositivo, imagen y QR. Calendario y Eventos comparten una agenda global con festivos manuales y nacionales/autonómicos de España. También incluye configuración de botones, refresco automático global en minutos y vista previa monocroma. Las imágenes se convierten a blanco y negro con dithering para conservar detalle. Los proyectos e imágenes se guardan en `.data/`, que Git ignora.

```powershell
npm test
npm run dev
```

Abre [http://localhost:51731](http://localhost:51731). La interfaz mostrará claramente **Simulación local**. Cambia los archivos de `web/` y recarga el navegador para ver los cambios.

La simulación y el firmware leen la misma `MI_EINK_OPENWEATHER_API_KEY` desde `include/secrets.h`. Como alternativa temporal, la simulación admite la variable `OPENWEATHER_API_KEY`, que tiene prioridad. La clave solo existe en el servidor: nunca se envía al navegador ni forma parte del proyecto exportado. El widget Tiempo combina las APIs de tiempo actual y previsión de 5 días / 3 horas, normaliza sus datos para la pantalla e‑ink y conserva cada lectura durante una hora. La interfaz comprueba si hay datos obsoletos cada minuto, pero el servidor no vuelve a llamar a OpenWeather antes de que pasen 60 minutos.

Los iconos se generan desde el set `material-symbols` de Iconify y quedan embebidos localmente para funcionar también sin Internet. Si se modifica la lista de iconos, regénéralos antes de compilar el firmware:

```powershell
npm run icons
```

El botón **Simular pantalla** genera el BMP final y reproduce los estados `queued`, `updating` y `ready`, pero no escribe en una pantalla física. La configuración Wi-Fi introducida en el simulador nunca guarda ni devuelve contraseñas.

En el controlador, el proyecto Studio se guarda de forma persistente en su memoria interna. Por tanto, al abrir `http://192.168.4.1` desde un móvil o un PC se carga el mismo panel, agenda, ajustes e imágenes reducidas. Las contraseñas no se guardan en el proyecto.

El controlador trabaja en modo **AP + STA**: mantiene siempre su red de configuración y, a la vez, se conecta al Wi‑Fi doméstico para consultar tiempo y festivos. En **Dispositivo > Red** se pueden escanear las redes cercanas, ordenar por señal y elegir una sin escribir manualmente el SSID. La credencial doméstica se guarda solo en la NVS del ESP32 y nunca se devuelve a la web.

## Configuración privada de Wi-Fi

La red del dispositivo se llama `MiEInk`. Su contraseña real no se publica en este repositorio: está en `include/secrets.h`, que Git ignora.

En un clon nuevo, crea tu archivo local a partir de la plantilla:

```powershell
Copy-Item include\secrets.example.h include\secrets.h
```

Edita `include/secrets.h` y define una contraseña WPA2 de al menos ocho caracteres y tu clave de OpenWeather:

```cpp
#define MI_EINK_WIFI_SSID "MiEInk"
#define MI_EINK_WIFI_PASSWORD "elige-una-clave-larga"
#define MI_EINK_HOME_WIFI_SSID ""       // opcional; también se elige desde Studio
#define MI_EINK_HOME_WIFI_PASSWORD ""   // opcional
#define MI_EINK_OPENWEATHER_API_KEY "pega-aqui-tu-clave"
```

Nunca subas `include/secrets.h`. La compilación se detiene con un mensaje claro si falta o si la clave del AP no cumple WPA2. `flash.ps1` avisa si falta OpenWeather: se puede flashear, pero el widget Tiempo quedará deshabilitado hasta añadir la clave y volver a cargar el firmware. Los festivos no necesitan clave.

## Instalar el firmware

1. Instala [Visual Studio Code](https://code.visualstudio.com/) y la extensión PlatformIO IDE.
2. Conecta el kit mediante un cable USB-C de datos y enciende el interruptor.
3. Abre esta carpeta completa en VS Code.
4. Crea `include/secrets.h` como se explica arriba.
5. Ejecuta o pulsa **Upload**:

```powershell
pio run -e trmnl_og_diy
pio run -e trmnl_og_diy -t upload
```

Para compilar y cargar de golpe, ejecuta desde PowerShell:

```powershell
.\flash.ps1
```

El script detecta automáticamente el XIAO ESP32-S3 por USB, regenera la web embebida y realiza la carga. También puedes abrir `flash.cmd` con doble clic. Si hay más de un XIAO, indica el puerto: `./flash.ps1 -Port COM6`.

Si PlatformIO no encuentra el puerto, mantén **BOOT**, pulsa y suelta **RESET**, suelta BOOT y repite la carga. Para indicar el puerto manualmente:

```powershell
pio device list
pio run -e trmnl_og_diy -t upload --upload-port COM6
```

Después conecta el móvil u ordenador al SSID definido en `MI_EINK_WIFI_SSID` y abre [http://192.168.4.1](http://192.168.4.1). Si todavía no configuraste la red doméstica y el teléfono avisa de que no hay Internet, elige permanecer conectado. Abre **Dispositivo**, pulsa **Buscar redes cercanas**, elige la de casa e introduce su contraseña.

## Uso y diagnóstico del panel

- **KEY1**: refresco completo en blanco.
- **KEY2**: refresco completo en negro.

Estas son todavía las funciones del firmware de diagnóstico. Las acciones configurables de KEY1, KEY2 y KEY3 en MiEInk Studio son una simulación para la siguiente fase y aún no cambian el controlador.

Para inspeccionar el diagnóstico USB, abre el [XIAO ESP32 Series Web Flasher](https://www.seeedstudio.com/xiao-series-page/playground/esp-flasher/) en Chrome o Edge, pulsa **Connect**, selecciona el XIAO y abre el monitor a `115200` baudios. El firmware informa de los pines, botones, estado `BUSY` y de cada fase de actualización. Un mensaje `[EPD][ERROR] BUSY no cambió` apunta a alimentación, cable FPC, puente o comunicación con el panel.

Antes de diagnosticar un panel sin respuesta, comprueba que el cable flexible está insertado y bloqueado, que su cara metálica mira hacia arriba y que el puente une `24Pin` con `GND`.

## API local

- `GET /api/status`: estado actual, resolución y resultado.
- `POST /api/display`: BMP sin compresión de 800 × 480, monocromo de un bit y exactamente 48.062 bytes.
- `GET /api/project` y `PUT /api/project`: proyecto completo persistido en el controlador, sin secretos.
- `POST /api/assets` y `GET/DELETE /api/assets/:id`: imágenes del proyecto.
- `GET /api/device/network`: estado seguro de AP + red doméstica, sin credenciales.
- `GET /api/device/networks`: escaneo de redes cercanas, señal y tipo de seguridad.
- `POST /api/device/network`: guarda la red doméstica en NVS y reconecta sin detener el AP.
- `GET /api/device/status`: telemetría de red, pantalla y sistema; no contiene secretos.
- `GET /api/integrations/geocode` y `GET /api/integrations/weather`: OpenWeather desde el controlador; la previsión se cachea durante una hora.
- `GET /api/integrations/holidays`: festivos nacionales y autonómicos de España desde Nager.Date v3, con caché persistente en LittleFS y uso sin conexión de la última copia válida.

El proyecto se guarda primero en LittleFS y el navegador mantiene únicamente una copia de respaldo. La reconexión Wi-Fi no comienza hasta que el guardado ha sido confirmado. Los widgets de reloj/fecha de la página activa pueden actualizarse de forma autónoma mediante NTP con el intervalo configurado; el panel usa refresco diferencial y fuerza uno completo cada 12 ciclos para limitar el ghosting.

Las respuestas son `202` al aceptar una actualización, `400` ante una imagen inválida, `409` si existe otra actualización y `500` si falla la memoria o la pantalla. La simulación local conserva el mismo contrato.

## Restaurar TRMNL

1. Conecta el kit por USB-C.
2. Abre [usetrmnl.com/flash](https://usetrmnl.com/flash) con Chrome o Edge.
3. Selecciona **TRMNL 7.5" (OG) DIY Kit** y completa el asistente.

## Licencia

[MIT](LICENSE).

## Referencias

- [TRMNL 7,5" (OG) DIY Kit — Seeed Studio](https://wiki.seeedstudio.com/trmnl_7inch5_diy_kit_main_page/)
- [Programación del kit con Arduino](https://wiki.seeedstudio.com/ogdiy_kit_works_with_arduino/)
- [Modo BOOT del XIAO ESP32-S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
