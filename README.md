<div align="center">

<!-- Reemplaza por el póster/render del badge cuando esté listo -->
<img src="docs/images/badge.png" alt="Badge AHAU CTF 2026" width="640">

# Badge AHAU CTF 2026

**El badge electrónico oficial del AHAU CTF 2026 — un ESP32-S3 con tira de LEDs, chat entre asistentes y herramientas de reconocimiento inalámbrico, todo en tu bolsillo.**

![Estado](https://img.shields.io/badge/estado-en_desarrollo-yellow)
![Licencia](https://img.shields.io/badge/licencia-GPLv3-blue)
![Plataforma](https://img.shields.io/badge/MCU-ESP32--S3-informational)
![Hecho en](https://img.shields.io/badge/Hecho_en-Mexico%20%F0%9F%87%B2%F0%9F%87%BD-green)

</div>

## Tabla de contenido

- [¿Qué es el Badge AHAU?](#qué-es-el-badge-ahau)
- [Características](#características)
- [Estructura del repositorio](#estructura-del-repositorio)
- [Hardware](#hardware)
- [Software](#software)
- [Instalación (flasheo)](#instalación-flasheo)
- [Uso](#uso)
  - [Modos del badge](#modos-del-badge)
  - [Controles](#controles)
  - [Indicador de modo (LED integrado)](#indicador-de-modo-led-integrado)
  - [Panel web y chat entre badges](#panel-web-y-chat-entre-badges)
- [Compilar desde el código fuente](#compilar-desde-el-código-fuente)
- [Solución de problemas](#solución-de-problemas)
- [Contribuir](#contribuir)
- [Créditos](#créditos)
- [Licencia](#licencia)

---

## ¿Qué es el Badge AHAU?

El **Badge AHAU** es el gafete electrónico que se entrega a los participantes del **AHAU CTF**. Más que un adorno luminoso, es una pequeña plataforma de aprendizaje y convivencia: trae una tira de LEDs programable, dos botones, y un puñado de modos que van desde efectos visuales hasta herramientas reales de reconocimiento Wi-Fi y BLE.

Todos los badges hablan entre sí: al encender el modo principal, cada uno levanta su propio punto de acceso Wi-Fi y abre un **chat grupal inalámbrico** (vía ESP-NOW) para que los asistentes conversen sin internet. Es hardware abierto, hackeable y pensado para que lo trastees.

## Características

- **Tira NeoPixel de 14 LEDs** con efectos: arcoíris, cometa y respiración.
- **Monitor de canales Wi-Fi** en tiempo real con detección de *beacons*, *probes* y *deauth*.
- **Escáner Wi-Fi** y **escáner BLE** para reconocimiento del entorno.
- **Punto de acceso + panel web** para configurar tu badge desde el teléfono.
- **Chat grupal entre badges** por ESP-NOW (sin internet, de dispositivo a dispositivo).
- **Modo Morse** que deletrea palabras del CTF con la tira de LEDs.
- **Mini-juego** de reflejos para matar el tiempo entre retos.
- **Modo identidad** para personalizar el nombre de tu badge.
- Todo controlado con **solo 2 botones** (MODO y ACCIÓN) y brillo ajustable.

## Estructura del repositorio

```
Badge_AHAU-CTF_26/
├── Hardware/                 # Diseño electrónico abierto
│   ├── AHAU_CTF26_BAdge.epro  # Proyecto fuente de EasyEDA Pro (esquemático + PCB)
│   ├── BOM_AHAU_CTF_Badge_2026.xlsx / .csv  # Lista de materiales
│   └── Gerber_AHAU_CTF.zip    # Archivos de fabricación
├── Software/
│   └── wifi_monitor/
│       └── wifi_monitor.ino  # Sketch de Arduino (código fuente del firmware)
├── Firmware/                 # Binarios precompilados + flasheadores
│   ├── firmware/             # .bin listos para flashear
│   ├── flash_unix.sh         # Flasheo automático (Linux / macOS)
│   ├── flash_windows.bat     # Flasheo automático (Windows)
│   └── LEEME.txt             # Guía de flasheo paso a paso
├── docs/                     # Imágenes y documentación
├── LICENSE                   # GPLv3
└── README.md
```

## Hardware

El badge se construye alrededor de un **ESP32-S3 SuperMini** (4 MB de flash). El diseño es abierto: en [`Hardware/`](Hardware/) está el **proyecto fuente de EasyEDA Pro** (`.epro`, con esquemático y PCB), la **BOM** (en `.xlsx` y `.csv`, con números de parte LCSC) y los **Gerbers** listos para fabricar. Consulta [`Hardware/README.md`](Hardware/README.md) para abrir el proyecto y mandar a fabricar la placa.

| Componente | Conexión | Detalle |
|---|---|---|
| MCU | — | ESP32-S3 (SuperMini / Dev Module), 4 MB flash |
| Tira de LEDs | `GPIO1` | 14 LEDs WS2812B direccionables |
| LED de estado | `GPIO48` | LED RGB integrado — indica el modo actual |
| Botón **MODO** | `GPIO8` | a GND, pull-up interno |
| Botón **ACCIÓN** | `GPIO9` | a GND, pull-up interno |
| Alimentación | USB-C / batería (conector PH2.0) | switch deslizante de encendido |

## Software

El firmware (`wifi_monitor.ino`) es un sketch de Arduino para el core **ESP32 de Espressif**. Es un bucle no bloqueante que gestiona los botones, pinta la tira de LEDs y conmuta entre modos. Las piezas principales:

- **Gestor de botones** con antirrebote y detección de pulsación larga.
- **Modos independientes** (Wi-Fi, arcoíris, Morse, juego, escáneres, AP, identidad), cada uno con su propia lógica de entrada/acción/actualización.
- **SoftAP + servidor web** (`WebServer`) para el panel de configuración.
- **Chat ESP-NOW** por *broadcast*, con una firma propia (`CHAT_MAGIC`) para ignorar tráfico ajeno.
- **Almacenamiento no volátil** (`Preferences`) para conservar el nombre del badge entre reinicios.

### Dependencias

- Core **esp32** de Espressif para Arduino (probado con la versión 3.3.7).
- Librería **Adafruit NeoPixel**.
- Esquema de particiones **"Huge APP (3MB No OTA/1MB SPIFFS)"** — necesario por la pila BLE.

## Instalación (flasheo)

El firmware precompilado está en [`Firmware/`](Firmware/). Hay tres caminos; elige el que te acomode. La guía completa está en [`Firmware/LEEME.txt`](Firmware/LEEME.txt).

### Opción A — Web flasher (sin instalar nada) ✅ *la más fácil*

1. Conecta el badge al PC por **USB-C** (usa un cable de **datos**, no de solo carga).
2. Abre en **Chrome o Edge** (no Firefox ni Safari): <https://esptool.spacehuhn.com/>
3. Pulsa **Connect** y elige el puerto (*USB Serial / JTAG / CDC*).
   - Si no aparece: mantén **BOOT**, toca **RESET**, suelta **BOOT** y vuelve a conectar.
4. Configura **una sola fila**: `Offset: 0x0` → archivo `Firmware/firmware/wifi_monitor.ino.merged.bin`
5. Pulsa **Program**, espera al 100 % y pulsa **RESET**. ¡Listo!

### Opción B — Script automático (Windows / Linux / macOS)

Requisito único: **Python 3**. El script instala `esptool` si hace falta.

```bash
# Linux / macOS
cd Firmware
./flash_unix.sh
```

```bat
REM Windows: doble clic en
Firmware\flash_windows.bat
```

### Opción C — Comando manual (avanzado)

```bash
pip install esptool
esptool --chip esp32s3 -b 921600 write_flash 0x0 Firmware/firmware/wifi_monitor.ino.merged.bin
```

## Uso

Al encender, el badge arranca en el **modo AP** (punto de acceso + arcoíris). Con los dos botones navegas entre todos los modos.

### Modos del badge

| Modo | Qué hace |
|---|---|
| **AP** *(inicial)* | Levanta un Wi-Fi `AHAU-<nombre>` con panel web y chat entre badges. La tira muestra arcoíris. |
| **Monitor Wi-Fi** | Visualiza la actividad por canal: *beacons* (azul), *probes* (verde), *deauth* (rojo). |
| **Arcoíris** | Efectos de luz: arcoíris, cometa, respiración (cambia con ACCIÓN). |
| **Morse** | Deletrea en Morse palabras del CTF con la tira de LEDs. |
| **Juego** | Mini-juego de reflejos. |
| **Escáner Wi-Fi** | Lista redes Wi-Fi cercanas. |
| **Escáner BLE** | Lista dispositivos Bluetooth LE cercanos. |
| **Identidad** | Muestra/configura la identidad visual del badge. |

### Controles

| Botón | Pulsación corta | Pulsación larga |
|---|---|---|
| **MODO** (`GPIO8`) | Siguiente modo | Cambia el brillo global de la tira |
| **ACCIÓN** (`GPIO9`) | Acción propia de cada modo | — |

### Indicador de modo (LED integrado)

El LED RGB integrado (`GPIO48`) muestra un color según el modo activo:

| Modo | Color |
|---|---|
| Monitor Wi-Fi | 🔵 Azul |
| Arcoíris | 🟣 Magenta |
| Morse | 🟦 Cian |
| Juego | 🟠 Ámbar |
| Escáner Wi-Fi | 🟢 Verde |
| Escáner BLE | 🔵 Azul |
| AP | ⚪ Blanco |
| Identidad | ⚫ Neutro (manda la tira) |

### Panel web y chat entre badges

En **modo AP**, el badge crea una red Wi-Fi con prefijo `AHAU-` (contraseña por defecto `ahauctf2026`). Conéctate desde el teléfono y abre el navegador para:

- Cambiar el **nombre** de tu badge (se guarda en memoria).
- Conmutar de modo desde la web.
- Usar el **chat grupal**: todos los badges en el mismo canal se escuchan por **ESP-NOW**, sin necesidad de internet ni router.

## Compilar desde el código fuente

1. Instala el [core ESP32 para Arduino](https://github.com/espressif/arduino-esp32).
2. Instala la librería **Adafruit NeoPixel** desde el gestor de librerías.
3. Abre [`Software/wifi_monitor/wifi_monitor.ino`](Software/wifi_monitor/wifi_monitor.ino).
4. Selecciona la placa **ESP32-S3 Dev Module** y el esquema de particiones **"Huge APP (3MB No OTA/1MB SPIFFS)"**.
5. Compila y sube.

## Solución de problemas

| Síntoma | Solución |
|---|---|
| No aparece el puerto | Usa un cable USB-C de **datos** (no de solo carga). |
| `Failed to connect` | Entra en modo descarga: mantén **BOOT**, toca **RESET**, suelta **BOOT**. |
| Windows sin driver | Instala el driver **CP210x** o **CH340** según tu placa. |
| No arranca tras flashear | Pulsa **RESET** una vez. |

## Contribuir

Las aportaciones son bienvenidas: mejoras al firmware, nuevos modos, correcciones de hardware o documentación. Abre un *issue* para discutir cambios grandes antes de mandar un *pull request*.

## Créditos

Proyecto desarrollado para la comunidad del **AHAU CTF** por [Lab217](https://github.com/Lab217MX).

## Licencia

Distribuido bajo la **Licencia Pública General de GNU v3 (GPLv3)**. Consulta el archivo [`LICENSE`](LICENSE) para más detalles.
