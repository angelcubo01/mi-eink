# MiEInk

Herramienta local para escribir un mensaje, previsualizarlo a 800 × 480 píxeles y enviarlo a un **TRMNL 7,5" (OG) DIY Kit** monocromo. No usa TRMNL Cloud, cuentas ni un servidor externo.

La interfaz se puede desarrollar completamente en el ordenador mediante una simulación local; solo necesitas flashear el XIAO cuando quieras probar el panel e-paper real.

> Cargar el firmware sustituye el firmware TRMNL instalado de fábrica. Puedes restaurarlo con el flasheador oficial de TRMNL.

## Hardware compatible

- TRMNL 7,5" (OG) DIY Kit monocromo de 800 × 480.
- XIAO ESP32-S3 Plus integrado en la placa EE04.
- Panel UC8179 conectado al conector de 24 pines.

No es compatible con la variante XIAO ESP32-C3 ni con los modelos de tres o seis colores.

## Desarrollo de la interfaz sin hardware

La simulación sirve la misma web y reproduce la API local del dispositivo. El botón **Actualizar pantalla** valida el BMP y simula los estados `queued`, `updating` y `ready`, pero no escribe en una pantalla física.

```powershell
npm test
npm run dev
```

Abre [http://localhost:5173](http://localhost:5173). La interfaz mostrará claramente **Simulación local · sin pantalla física**. Cambia los archivos de `web/` y recarga el navegador para ver los cambios.

## Configuración privada de Wi-Fi

La red del dispositivo se llama `MiEInk`. Su contraseña real no se publica en este repositorio: está en `include/secrets.h`, que Git ignora.

En un clon nuevo, crea tu archivo local a partir de la plantilla:

```powershell
Copy-Item include\secrets.example.h include\secrets.h
```

Edita `include/secrets.h` y define una contraseña WPA2 de al menos ocho caracteres:

```cpp
#define MI_EINK_WIFI_SSID "MiEInk"
#define MI_EINK_WIFI_PASSWORD "elige-una-clave-larga"
```

Nunca subas `include/secrets.h`. La compilación se detiene con un mensaje claro si falta.

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

Si PlatformIO no encuentra el puerto, mantén **BOOT**, pulsa y suelta **RESET**, suelta BOOT y repite la carga. Para indicar el puerto manualmente:

```powershell
pio device list
pio run -e trmnl_og_diy -t upload --upload-port COM6
```

Después conecta el móvil u ordenador a la red `MiEInk` y abre [http://192.168.4.1](http://192.168.4.1). Si el teléfono avisa de que la red no tiene Internet, elige permanecer conectado.

## Uso y diagnóstico del panel

- **KEY1**: refresco completo en blanco.
- **KEY2**: refresco completo en negro.

Para inspeccionar el diagnóstico USB, abre el [XIAO ESP32 Series Web Flasher](https://www.seeedstudio.com/xiao-series-page/playground/esp-flasher/) en Chrome o Edge, pulsa **Connect**, selecciona el XIAO y abre el monitor a `115200` baudios. El firmware informa de los pines, botones, estado `BUSY` y de cada fase de actualización. Un mensaje `[EPD][ERROR] BUSY no cambió` apunta a alimentación, cable FPC, puente o comunicación con el panel.

Antes de diagnosticar un panel sin respuesta, comprueba que el cable flexible está insertado y bloqueado, que su cara metálica mira hacia arriba y que el puente une `24Pin` con `GND`.

## API local

- `GET /api/status`: estado actual, resolución y resultado.
- `POST /api/display`: BMP sin compresión de 800 × 480, monocromo de un bit y exactamente 48.062 bytes.

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
