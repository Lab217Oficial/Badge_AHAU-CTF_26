/*
 * AHAU CTF Badge - ESP32-S3 SuperMini
 * Firmware de badge con multiples modos controlados por 2 botones.
 *
 *   - Tira NeoPixel de 14 LEDs .......... GPIO1
 *   - LED RGB integrado (WS2812) ........ GPIO48  (indicador de modo)
 *   - Boton MODO ........................ GPIO8   (a GND, usa pull-up interno)
 *   - Boton ACCION ...................... GPIO9   (a GND, usa pull-up interno)
 *
 * Controles:
 *   MODO   - pulsacion corta: siguiente modo
 *          - pulsacion larga: cambia el brillo global
 *   ACCION - funcion propia de cada modo
 *
 * Modos: WiFi | Arcoiris | Morse | Juego | Escaner WiFi | Escaner BLE | AP
 *
 * Requiere: Adafruit NeoPixel library
 * Board: ESP32-S3 (Dev Module)
 * Particion: "Huge APP (3MB No OTA/1MB SPIFFS)" (necesaria por la pila BLE)
 */

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <Adafruit_NeoPixel.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ======================= Configuracion de pines =======================
#define NEOPIXEL_PIN    1     // Tira principal de 14 LEDs
#define NUM_PIXELS      14
#define ONBOARD_PIN     48    // LED RGB integrado del SuperMini
#define BTN_MODE_PIN    8     // Boton: cambiar de modo
#define BTN_ACTION_PIN  9     // Boton: accion del modo

// ======================= Parametros generales =========================
#define FADE_AMOUNT     2
#define MIN_BRIGHTNESS  5
#define MORSE_UNIT      150   // ms - unidad base de tiempo Morse

// Anillo RGBW (SK6812): 4 bytes por LED. Si fuera WS2812 RGB, usar NEO_GRB.
Adafruit_NeoPixel pixels(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRBW + NEO_KHZ800);
Adafruit_NeoPixel onboard(1, ONBOARD_PIN, NEO_GRB + NEO_KHZ800);

// Niveles de brillo de la tira (ciclables con MODO-largo).
// Bajos a proposito: menos corriente (evita brownout) y menos agresivo a la vista.
const uint8_t brightnessLevels[] = {20, 50, 100};
uint8_t brightnessIndex = 1;

// ---- Modo AP + panel web ----
#define AP_PREFIX "AHAU-"           // prefijo comun: todos los badges empiezan igual
#define AP_PASS   "ahauctf2026"     // >=8 caracteres (WPA2). Pon "" para AP abierto.
#define ESPNOW_CHANNEL 1            // canal comun del SoftAP para que el chat ESP-NOW se oiga entre badges
WebServer server(80);
Preferences prefs;                  // almacenamiento no volatil (nombre del badge)
String badgeName;                   // parte unica/legible del SSID
String apSSID;                      // SSID completo = AP_PREFIX + badgeName
int  pendingMode = -1;              // cambio de modo pedido desde la web (aplicado en loop)
bool pendingApRestart = false;      // reiniciar el AP con nuevo SSID (aplicado en loop)

// ======================= Botones (no bloqueante) ======================
struct Button {
  uint8_t  pin;
  bool     pressed;      // estado ya "debounced" (true = presionado)
  bool     lastReading;  // ultima lectura cruda
  uint32_t lastChange;   // ms del ultimo cambio de lectura cruda
  uint32_t pressStart;   // ms en que empezo la pulsacion
  bool     fell;         // flanco: recien presionado (este tick)
  bool     rose;         // flanco: recien soltado (este tick)
  bool     longFired;    // ya se disparo la pulsacion larga
  bool     longEvent;    // pulsacion larga detectada (este tick)
};

Button btnMode;
Button btnAction;

void buttonInit(Button &b, uint8_t pin) {
  b.pin = pin;
  pinMode(pin, INPUT_PULLUP);   // boton entre pin y GND -> presionado = LOW
  b.pressed = false;
  b.lastReading = false;
  b.lastChange = 0;
  b.pressStart = 0;
  b.fell = false;
  b.rose = false;
  b.longFired = false;
  b.longEvent = false;
}

void buttonUpdate(Button &b, uint32_t now) {
  b.fell = false;
  b.rose = false;
  b.longEvent = false;

  bool raw = (digitalRead(b.pin) == LOW);   // activo en bajo

  if (raw != b.lastReading) {
    b.lastReading = raw;
    b.lastChange = now;
  }

  // Aceptar el cambio solo si la lectura se mantuvo estable 20 ms
  if ((now - b.lastChange) > 20 && raw != b.pressed) {
    b.pressed = raw;
    if (raw) {
      b.fell = true;
      b.pressStart = now;
      b.longFired = false;
    } else {
      b.rose = true;
    }
  }

  // Pulsacion larga (600 ms manteniendo presionado)
  if (b.pressed && !b.longFired && (now - b.pressStart) > 600) {
    b.longEvent = true;
    b.longFired = true;
  }
}

// ======================= Modos ========================================
enum {
  MODE_AP = 0,       // principal: SoftAP + panel web (la tira muestra arcoiris)
  MODE_WIFI,         // monitor de canales (siguiente al AP)
  MODE_RAINBOW,
  MODE_MORSE,
  MODE_GAME,
  MODE_WIFISCAN,
  MODE_BLESCAN,
  MODE_IDENTITY,
  MODE_COUNT
};
int currentMode = MODE_AP;        // arranca en AP + arcoiris; el siguiente modo es el monitor

// Color del LED integrado segun el modo (indicador visual, brillo bajo)
uint32_t modeColor(int m) {
  switch (m) {
    case MODE_WIFI:    return onboard.Color(0, 12, 24);   // azul
    case MODE_RAINBOW: return onboard.Color(18, 0, 22);   // magenta
    case MODE_MORSE:    return onboard.Color(0, 22, 18);  // cian
    case MODE_GAME:     return onboard.Color(24, 12, 0);  // ambar
    case MODE_WIFISCAN: return onboard.Color(0, 22, 6);   // verde
    case MODE_BLESCAN:  return onboard.Color(0, 8, 22);   // azul
    case MODE_AP:       return onboard.Color(20, 20, 20); // blanco
    case MODE_IDENTITY: return onboard.Color(6, 6, 6);    // neutro (la tira manda)
  }
  return 0;
}

void showOnboard(int m) {
  onboard.setPixelColor(0, modeColor(m));
  onboard.show();
}

// Utilidad: rueda de color (0-255 -> RGB)
uint32_t Wheel(byte pos) {
  pos = 255 - pos;
  if (pos < 85) {
    return pixels.Color(255 - pos * 3, 0, pos * 3);
  } else if (pos < 170) {
    pos -= 85;
    return pixels.Color(0, pos * 3, 255 - pos * 3);
  } else {
    pos -= 170;
    return pixels.Color(pos * 3, 255 - pos * 3, 0);
  }
}

// ======================= MODO WiFi (monitor de canales) ===============
// pixel i  <->  canal (i+1). Color = tipo de trama mas reciente; BRILLO =
// ACTIVIDAD (volumen de trafico) del canal: cada paquete sube la actividad y
// esta decae suave cuando el trafico baja. Asi un canal saturado (un beacon
// spam, o un canal muy usado) se ve MUCHO mas brillante que uno tranquilo,
// aunque el RSSI sea parecido. Un punto blanco tenue (canal W del SK6812)
// marca el canal que el escaner esta "mirando" ahora.
enum { KIND_NONE = 0, KIND_BEACON, KIND_PROBE, KIND_DEAUTH, KIND_OTHER };

#define DWELL_MS     220     // ms en cada canal antes de saltar
#define FRAME_MS     33      // ~30 fps: animacion suave, sin parpadeo
#define ACT_PER_PKT  30      // cuanto sube la actividad por paquete (satura en 255)
#define ACT_DECAY    4       // cuanto baja la actividad por frame (fade suave)

volatile uint8_t  chActivity[NUM_PIXELS] = {0};   // 0..255: volumen de trafico por canal
volatile uint8_t  chLastKind[NUM_PIXELS] = {0};   // ultimo tipo de trama visto (color)
volatile uint32_t chAlertUntil[NUM_PIXELS] = {0}; // deauth: rojo fijo hasta este millis()
uint8_t  currentChannel = 1;
bool     wifiLock = false;                         // ACCION: fija el canal actual (no salta)
volatile uint32_t totalPackets = 0;
volatile uint32_t cntBeacon = 0, cntProbe = 0, cntDeauth = 0, cntOther = 0;
volatile uint32_t deauthCh = 0;

// Callback de paquetes capturados en modo promiscuo (solo management frames).
// Corre en la tarea WiFi: mantenerlo breve. Clasifica el tipo y SUMA actividad.
void IRAM_ATTR snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  totalPackets++;

  uint8_t channel = pkt->rx_ctrl.channel;
  if (channel < 1 || channel > NUM_PIXELS) return;
  uint8_t i = channel - 1;
  uint8_t subtype = (pkt->payload[0] & 0xF0) >> 4;

  if (subtype == 10 || subtype == 12) {          // Deauth/Disassoc -> ROJO (alerta)
    chLastKind[i] = KIND_DEAUTH;
    chAlertUntil[i] = millis() + 3000;           // rojo fijo ~3 s para alcanzar a verlo
    cntDeauth++; deauthCh = channel;
  } else if (subtype == 8) {                     // Beacon -> AZUL
    chLastKind[i] = KIND_BEACON; cntBeacon++;
  } else if (subtype == 4 || subtype == 5) {     // Probe req/resp -> VERDE
    chLastKind[i] = KIND_PROBE;  cntProbe++;
  } else {                                       // Otros mgmt -> AMARILLO
    chLastKind[i] = KIND_OTHER;  cntOther++;
  }

  uint16_t a = (uint16_t)chActivity[i] + ACT_PER_PKT;   // acumula volumen
  chActivity[i] = (a > 255) ? 255 : (uint8_t)a;
}

void decayActivity() {
  for (int i = 0; i < NUM_PIXELS; i++) {
    if (chActivity[i] > ACT_DECAY) chActivity[i] -= ACT_DECAY;
    else chActivity[i] = 0;
  }
}

void wifiRender() {
  uint32_t nowms = millis();
  for (int i = 0; i < NUM_PIXELS; i++) {
    uint8_t b = chActivity[i];
    uint8_t r = 0, g = 0, bl = 0, w = 0;
    if (nowms < chAlertUntil[i]) {               // deauth activo: rojo fijo, bien visible
      r = 220;
    } else {
      switch (chLastKind[i]) {
        case KIND_BEACON: bl = b;         break;  // AZUL
        case KIND_PROBE:  g  = b;         break;  // VERDE
        case KIND_DEAUTH: r  = b;         break;  // ROJO residual
        case KIND_OTHER:  r  = b; g = b;  break;  // AMARILLO
        default:                          break;  // sin datos: apagado
      }
    }
    if ((i + 1) == currentChannel) w = 24;       // cursor: canal en observacion (blanco tenue)
    pixels.setPixelColor(i, pixels.Color(r, g, bl, w));
  }
  pixels.show();
}

void hopChannel() {
  currentChannel = (currentChannel % 13) + 1;    // recorre canales 1..13
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

// Activa (o reactiva) la captura promiscua para el monitor de canales.
void startPromiscuous() {
  esp_wifi_set_promiscuous(true);
  wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(snifferCallback);
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

void wifiEnter() {
  WiFi.mode(WIFI_STA);
  delay(50);                                     // deja arrancar el driver antes del promiscuo
  currentChannel = 1;
  wifiLock = false;
  for (int i = 0; i < NUM_PIXELS; i++) {
    chActivity[i] = 0; chLastKind[i] = KIND_NONE; chAlertUntil[i] = 0;
  }
  startPromiscuous();
  Serial.println("[WiFi] monitor ON  brillo=actividad · AZUL=beacon VERDE=probe ROJO=DEAUTH AMARILLO=otros");
}

void wifiAction() {
  wifiLock = !wifiLock;                           // fijar / soltar el canal actual
  Serial.printf("[WiFi] %s (canal %u)\n", wifiLock ? "canal FIJADO" : "escaneo", (unsigned)currentChannel);
}

void wifiUpdate(uint32_t now) {
  static uint32_t lastHop = 0, lastFrame = 0, lastStats = 0;

  if (!wifiLock && now - lastHop >= DWELL_MS) { hopChannel(); lastHop = now; }
  if (now - lastFrame >= FRAME_MS) { decayActivity(); wifiRender(); lastFrame = now; }

  if (deauthCh) {                                 // aviso inmediato de deauth por serial
    Serial.printf("!! DEAUTH/DISASSOC en canal %u\n", (unsigned)deauthCh);
    deauthCh = 0;
  }
  if (now - lastStats >= 2000) {                  // resumen periodico
    lastStats = now;
    Serial.printf("[stats] beacon=%lu probe=%lu deauth=%lu otros=%lu  (canal %u%s)\n",
                  (unsigned long)cntBeacon, (unsigned long)cntProbe,
                  (unsigned long)cntDeauth, (unsigned long)cntOther,
                  (unsigned)currentChannel, wifiLock ? " FIJO" : "");
  }
}

// ======================= MODO Arcoiris ================================
int      rbStyle = 0;   // 0 = arcoiris, 1 = cometa, 2 = respiracion
uint16_t rbJ = 0;
int      rbPos = 0;
int      rbDir = 1;
const char* rbStyleName[] = {"arcoiris", "cometa", "respiracion"};

void rainbowEnter() {
  rbJ = 0; rbPos = 0; rbDir = 1;
  pixels.clear(); pixels.show();
}

void rainbowAction() {
  rbStyle = (rbStyle + 1) % 3;
  pixels.clear(); pixels.show();
  Serial.printf("[Arcoiris] estilo: %s\n", rbStyleName[rbStyle]);
}

void rainbowUpdate(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 30) return;
  last = now;

  if (rbStyle == 0) {                 // Arcoiris deslizante
    for (int i = 0; i < NUM_PIXELS; i++)
      pixels.setPixelColor(i, Wheel((i * 256 / NUM_PIXELS + rbJ) & 255));
    rbJ = (rbJ + 3) & 255;

  } else if (rbStyle == 1) {          // Cometa que rebota
    for (int i = 0; i < NUM_PIXELS; i++) {
      uint32_t c = pixels.getPixelColor(i);
      uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
      pixels.setPixelColor(i, pixels.Color(r * 3 / 4, g * 3 / 4, b * 3 / 4));
    }
    pixels.setPixelColor(rbPos, Wheel(rbJ & 255));
    rbPos += rbDir;
    if (rbPos >= NUM_PIXELS - 1 || rbPos <= 0) rbDir = -rbDir;
    rbJ = (rbJ + 7) & 255;

  } else {                            // Respiracion (todo un color pulsando)
    float phase = (now % 4000) / 4000.0;
    float level = 1.0 - fabs(2 * phase - 1);   // triangulo 0..1
    uint32_t c = Wheel((now / 40) & 255);
    uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    for (int i = 0; i < NUM_PIXELS; i++)
      pixels.setPixelColor(i, pixels.Color(r * level, g * level, b * level));
  }
  pixels.show();
}

// ======================= MODO Morse ===================================
const char* codigoMorse[] = {
  ".-","-...","-.-.","-..",".","..-.","--.","....","..",".---","-.-",".-..","--",
  "-.","---",".--.","--.-",".-.","...","-","..-","...-",".--","-..-","-.--","--.."
};
const char* codigoMorseNum[] = {
  "-----",".----","..---","...--","....-",".....","-....","--...","---..","----."
};
const char* palabras[] = { "AHAU", "DELFIN", "GLITCHBOI", "YUCATAN", "CTF" };
const int numPalabras = sizeof(palabras) / sizeof(palabras[0]);

String   palabraActual = "";
int      letraIndex = 0;
int      simboloIndex = 0;
String   morseActual = "";
uint32_t lastMorseTime = 0;
bool     ledEncendido = false;
int      estadoMorse = 0;   // 0=simbolo, 1=pausa simbolo, 2=pausa letra, 3=pausa palabra
String   customWord = "";   // mensaje/flag enviado desde la web (vacio = palabras al azar)

String getMorseForChar(char c) {
  c = toupper(c);
  if (c >= 'A' && c <= 'Z') return String(codigoMorse[c - 'A']);
  if (c >= '0' && c <= '9') return String(codigoMorseNum[c - '0']);
  return "";
}

void elegirPalabraAlAzar() {
  if (customWord.length() > 0) palabraActual = customWord;
  else                        palabraActual = palabras[random(numPalabras)];
  letraIndex = 0;
  simboloIndex = 0;
  morseActual = "";
  estadoMorse = 0;
  ledEncendido = false;
  Serial.printf("[Morse] palabra: %s\n", palabraActual.c_str());
}

// Maquina de estados de Morse (no bloqueante). Solo cambia 'ledEncendido';
// el pintado en la tira lo hace morseUpdate().
void actualizarMorse() {
  uint32_t ahora = millis();

  if (palabraActual.length() == 0) { elegirPalabraAlAzar(); return; }

  if (letraIndex >= (int)palabraActual.length()) {
    if (ahora - lastMorseTime >= MORSE_UNIT * 7) elegirPalabraAlAzar();
    return;
  }

  if (morseActual.length() == 0) {
    morseActual = getMorseForChar(palabraActual.charAt(letraIndex));
    simboloIndex = 0;
    if (morseActual.length() == 0) { letraIndex++; lastMorseTime = ahora; return; }
  }

  switch (estadoMorse) {
    case 0:   // Mostrar simbolo (LED encendido)
      if (!ledEncendido) {
        ledEncendido = true;
        lastMorseTime = ahora;
      } else {
        int dur = (morseActual.charAt(simboloIndex) == '.') ? 1 : 3;
        if (ahora - lastMorseTime >= MORSE_UNIT * dur) {
          ledEncendido = false;
          lastMorseTime = ahora;
          simboloIndex++;
          estadoMorse = 1;
        }
      }
      break;

    case 1:   // Pausa entre simbolos (1 unidad)
      if (ahora - lastMorseTime >= MORSE_UNIT) {
        estadoMorse = (simboloIndex < (int)morseActual.length()) ? 0 : 2;
        if (estadoMorse == 2) lastMorseTime = ahora;
      }
      break;

    case 2:   // Pausa entre letras (3 unidades, ya paso 1)
      if (ahora - lastMorseTime >= MORSE_UNIT * 2) {
        letraIndex++;
        morseActual = "";
        simboloIndex = 0;
        estadoMorse = 0;
        lastMorseTime = ahora;
      }
      break;
  }
}

void morseEnter()  { elegirPalabraAlAzar(); }
void morseAction() { elegirPalabraAlAzar(); }   // saltar a otra palabra

void morseUpdate(uint32_t now) {
  actualizarMorse();
  uint32_t c = ledEncendido ? pixels.Color(0, 180, 180) : 0;
  for (int i = 0; i < NUM_PIXELS; i++) pixels.setPixelColor(i, c);
  pixels.show();
}

// ======================= MODO Juego: "Atrapa la Luz" ==================
// Una luz rebota por la tira; presiona ACCION para atraparla dentro de la
// zona verde. Acierto -> sube nivel (mas rapido, zona mas chica).
// Fallo -> muestra tu puntaje y vuelve al inicio.
enum { G_IDLE = 0, G_RUN, G_FLASH_WIN, G_FLASH_LOSE };
int      gState = G_IDLE;
int      gScore = 0;
int      gLevel = 0;
int      gRunPos = 0, gRunDir = 1;
int      gZoneCenter = 7, gZoneHalf = 2;
uint32_t gStepInterval = 140;
uint32_t gLastStep = 0;
uint32_t gFlashUntil = 0;

void gameSetupRound() {
  gZoneHalf = 2 - gLevel / 3;                 // empieza en +-2 y se encoge
  if (gZoneHalf < 0) gZoneHalf = 0;
  int minC = gZoneHalf, maxC = NUM_PIXELS - 1 - gZoneHalf;
  gZoneCenter = random(minC, maxC + 1);
  gStepInterval = 140 - (uint32_t)gLevel * 8; // acelera con el nivel
  if ((int32_t)gStepInterval < 35) gStepInterval = 35;
  gRunPos = 0; gRunDir = 1;
  gLastStep = millis();
}

void gameEnter() {
  gState = G_IDLE; gScore = 0; gLevel = 0;
  pixels.clear(); pixels.show();
}

void gameAction() {
  uint32_t now = millis();
  if (gState == G_IDLE) {
    gScore = 0; gLevel = 0;
    gameSetupRound();
    gState = G_RUN;
  } else if (gState == G_RUN) {
    if (abs(gRunPos - gZoneCenter) <= gZoneHalf) {
      gScore++; gLevel++;
      gState = G_FLASH_WIN; gFlashUntil = now + 250;
      Serial.printf("[Juego] acierto! puntaje: %d\n", gScore);
    } else {
      gState = G_FLASH_LOSE; gFlashUntil = now + 900;
      Serial.printf("[Juego] fallo. puntaje final: %d\n", gScore);
    }
  }
}

void gameUpdate(uint32_t now) {
  if (gState == G_IDLE) {
    // Invitacion: dos LEDs centrales respirando en azul
    float phase = (now % 1500) / 1500.0;
    float level = 1.0 - fabs(2 * phase - 1);
    uint8_t v = (uint8_t)(level * 120);
    pixels.clear();
    pixels.setPixelColor(NUM_PIXELS / 2, pixels.Color(0, 0, v));
    pixels.setPixelColor(NUM_PIXELS / 2 - 1, pixels.Color(0, 0, v / 2));
    pixels.show();

  } else if (gState == G_RUN) {
    if (now - gLastStep >= gStepInterval) {
      gRunPos += gRunDir;
      if (gRunPos >= NUM_PIXELS - 1) { gRunPos = NUM_PIXELS - 1; gRunDir = -1; }
      else if (gRunPos <= 0)         { gRunPos = 0;             gRunDir = 1;  }
      gLastStep = now;
    }
    pixels.clear();
    for (int i = gZoneCenter - gZoneHalf; i <= gZoneCenter + gZoneHalf; i++)
      if (i >= 0 && i < NUM_PIXELS) pixels.setPixelColor(i, pixels.Color(0, 60, 0));
    pixels.setPixelColor(gRunPos, pixels.Color(160, 160, 160));   // la "luz"
    pixels.show();

  } else if (gState == G_FLASH_WIN) {
    for (int i = 0; i < NUM_PIXELS; i++) pixels.setPixelColor(i, pixels.Color(0, 120, 0));
    pixels.show();
    if (now >= gFlashUntil) { gameSetupRound(); gState = G_RUN; }

  } else { // G_FLASH_LOSE: parpadea rojo y muestra el puntaje en LEDs
    if ((now / 150) % 2 == 0) {
      for (int i = 0; i < NUM_PIXELS; i++) pixels.setPixelColor(i, pixels.Color(120, 0, 0));
    } else {
      pixels.clear();
      for (int i = 0; i < gScore && i < NUM_PIXELS; i++)
        pixels.setPixelColor(i, pixels.Color(60, 60, 0));
    }
    pixels.show();
    if (now >= gFlashUntil) gState = G_IDLE;
  }
}

// ======================= Firma visual del badge ======================
// A partir del nombre del badge (la misma identidad del SSID) derivamos un
// color y un patron UNICOS y deterministas, para reconocer el badge de un
// vistazo sin abrir el WiFi. Si renombras el badge, su firma cambia con el.
uint8_t  sigHue1 = 0, sigHue2 = 128;
uint8_t  sigPattern = 0;
uint8_t  sigSpeed = 3;            // 2..5
uint16_t sigFrame = 0;
const uint8_t SIG_PATTERNS = 5;

uint32_t hashStr(const String& s) {          // FNV-1a de 32 bits
  uint32_t h = 2166136261UL;
  for (uint16_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s.charAt(i); h *= 16777619UL; }
  return h;
}

void computeSignature() {
  uint32_t h = hashStr(badgeName);
  sigHue1 = h & 0xFF;
  sigHue2 = (h >> 8) & 0xFF;
  uint8_t diff = sigHue1 - sigHue2;          // asegurar contraste entre tonos
  if (diff < 40 || diff > 216) sigHue2 = sigHue1 + 110;
  sigPattern = (h >> 16) % SIG_PATTERNS;
  sigSpeed = 2 + ((h >> 24) & 0x03);         // 2..5
  Serial.printf("[Firma] hue1=%u hue2=%u patron=%u vel=%u\n",
                sigHue1, sigHue2, sigPattern, sigSpeed);
}

// Dibuja la firma del badge en la tira (~30 fps). Reutilizada por el modo
// Identidad y por el efecto FX_IDENTITY del AP.
void identityUpdate(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 33) return;
  last = now; sigFrame++;

  uint32_t c1 = Wheel(sigHue1), c2 = Wheel(sigHue2);
  uint8_t r1=(c1>>16)&0xFF, g1=(c1>>8)&0xFF, b1=c1&0xFF;
  uint8_t r2=(c2>>16)&0xFF, g2=(c2>>8)&0xFF, b2=c2&0xFF;

  float s = sigSpeed / 3.0;                  // factor de velocidad (suave)
  switch (sigPattern) {
    case 0: {                                // respiracion suave en color 1
      float lv = 0.15 + 0.85 * (0.5 - 0.5 * cosf(sigFrame * 0.05 * s));
      for (int i=0;i<NUM_PIXELS;i++) pixels.setPixelColor(i, pixels.Color(r1*lv,g1*lv,b1*lv));
      break; }
    case 1: {                                // crossfade lento color1 <-> color2
      float f = 0.5 - 0.5 * cosf(sigFrame * 0.04 * s);
      for (int i=0;i<NUM_PIXELS;i++)
        pixels.setPixelColor(i, pixels.Color(r1*(1-f)+r2*f, g1*(1-f)+g2*f, b1*(1-f)+b2*f));
      break; }
    case 2: {                                // gradiente rotatorio suave color1/color2
      float off = sigFrame * 0.03 * s;
      for (int i=0;i<NUM_PIXELS;i++) {
        float f = 0.5 + 0.5 * cosf((float)i / NUM_PIXELS * 6.2832 + off);
        pixels.setPixelColor(i, pixels.Color(r1*f+r2*(1-f), g1*f+g2*(1-f), b1*f+b2*(1-f)));
      }
      break; }
    case 3: {                                // punto suave con estela (vaiven, sin parpadeo)
      float pos = (NUM_PIXELS - 1) * (0.5 - 0.5 * cosf(sigFrame * 0.03 * s));
      for (int i=0;i<NUM_PIXELS;i++) {
        float d = fabs(i - pos); float lv = d < 3 ? (1.0 - d / 3.0) : 0;
        pixels.setPixelColor(i, pixels.Color(r1*lv, g1*lv, b1*lv));
      }
      break; }
    default: {                               // dos mitades que respiran en contrafase
      float f = 0.5 - 0.5 * cosf(sigFrame * 0.04 * s);
      for (int i=0;i<NUM_PIXELS;i++) {
        float lv = (i < NUM_PIXELS/2) ? f : (1 - f);
        uint32_t c = (i < NUM_PIXELS/2) ? c1 : c2;
        pixels.setPixelColor(i, pixels.Color(((c>>16)&0xFF)*lv, ((c>>8)&0xFF)*lv, (c&0xFF)*lv));
      }
      break; }
  }
  pixels.show();
}

// ======================= MODO Escaner WiFi ============================
// Lista las redes cercanas (SSID, canal, señal, cifrado). Solo RECIBE los
// beacons que los AP emiten publicamente; no transmite ni ataca nada.
struct ApInfo { char ssid[33]; int8_t rssi; uint8_t ch; bool open; };
ApInfo   apList[NUM_PIXELS];
int      apCount = 0;
bool     scanRunning = false;
uint32_t scanLastEnd = 0;
const uint32_t SCAN_PERIOD = 4000;

void scanRender() {
  pixels.clear();
  for (int i = 0; i < apCount; i++) {
    uint32_t c;
    if      (apList[i].open)        c = pixels.Color(0, 0, 150);   // abierta -> azul
    else if (apList[i].rssi > -55)  c = pixels.Color(0, 120, 0);   // fuerte  -> verde
    else if (apList[i].rssi > -70)  c = pixels.Color(90, 70, 0);   // media   -> ambar
    else                            c = pixels.Color(120, 0, 0);   // debil   -> rojo
    pixels.setPixelColor(i, c);
  }
  pixels.show();
}

void scanStart() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true);          // asincrono: no bloquea el loop
  scanRunning = true;
}

void scanEnter() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  apCount = 0;
  scanRunning = false;
  scanLastEnd = 0;                  // dispara un escaneo de inmediato
}

void scanExit() {
  WiFi.scanDelete();
  scanRunning = false;
}

void scanAction() {                 // ACCION: re-escanear ya
  if (!scanRunning) scanStart();
}

void scanUpdate(uint32_t now) {
  int n = WiFi.scanComplete();
  if (n >= 0) {                     // escaneo terminado
    apCount = (n > NUM_PIXELS) ? NUM_PIXELS : n;
    for (int i = 0; i < apCount; i++) {
      WiFi.SSID(i).toCharArray(apList[i].ssid, sizeof(apList[i].ssid));
      apList[i].rssi = WiFi.RSSI(i);
      apList[i].ch   = WiFi.channel(i);
      apList[i].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();
    scanRunning = false;
    scanLastEnd = now;
    scanRender();
    Serial.printf("[ScanWiFi] %d redes\n", n);
  } else if (!scanRunning && (scanLastEnd == 0 || now - scanLastEnd > SCAN_PERIOD)) {
    scanStart();
  }
  if (scanRunning) {                // barrido "buscando" mientras escanea
    static uint32_t last = 0; static int p = 0;
    if (now - last > 60) {
      last = now; pixels.clear();
      pixels.setPixelColor(p % NUM_PIXELS, pixels.Color(20, 20, 20));
      pixels.show(); p++;
    }
  }
}

// ======================= MODO Escaner BLE =============================
// Escucha PASIVAMENTE los advertisements BLE de los dispositivos cercanos y
// los lista. No se conecta ni transmite: solo recibe. La pila BLE se inicia
// al entrar y se libera al salir para no gastar RAM de mas.
struct BleInfo { char name[24]; int rssi; };
BleInfo  bleList[NUM_PIXELS];
volatile int bleCount = 0;
BLEScan* pBLEScan = nullptr;
bool     bleScanning = false;
uint32_t bleLastEnd = 0;
const uint32_t BLE_SCAN_SECS = 3;

class BleCollect : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (bleCount >= NUM_PIXELS) return;
    String n = dev.getName().c_str();
    if (n.length() == 0) n = dev.getAddress().toString().c_str();
    n.toCharArray(bleList[bleCount].name, sizeof(bleList[bleCount].name));
    bleList[bleCount].rssi = dev.getRSSI();
    bleCount++;
  }
};
BleCollect bleCB;                   // instancia unica (evita fugas de memoria)

void bleScanDone(BLEScanResults results) {
  bleScanning = false;
  bleLastEnd = millis();
  if (pBLEScan) pBLEScan->clearResults();
}

void bleRender() {
  pixels.clear();
  for (int i = 0; i < bleCount; i++) {
    uint32_t c;
    if      (bleList[i].rssi > -60) c = pixels.Color(0, 80, 150);  // cerca
    else if (bleList[i].rssi > -75) c = pixels.Color(0, 40, 90);   // medio
    else                            c = pixels.Color(20, 0, 60);   // lejos
    pixels.setPixelColor(i, c);
  }
  pixels.show();
}

void bleStartScan() {
  bleCount = 0;
  bleScanning = true;
  pBLEScan->start(BLE_SCAN_SECS, bleScanDone, false);   // asincrono
}

void bleEnter() {
  WiFi.mode(WIFI_OFF);              // liberar RAM del WiFi para la pila BLE
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(&bleCB, false);
  pBLEScan->setActiveScan(false);   // escaneo PASIVO: solo escucha
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(80);
  bleLastEnd = 0;
  bleScanning = false;
  bleStartScan();
}

void bleExit() {
  if (pBLEScan) pBLEScan->stop();
  BLEDevice::deinit(true);          // liberar toda la RAM de BLE
  pBLEScan = nullptr;
  bleScanning = false;
  WiFi.mode(WIFI_OFF);              // el siguiente modo encendera el radio si lo necesita
}

void bleAction() {                  // ACCION: re-escanear ya
  if (!bleScanning && pBLEScan) bleStartScan();
}

void bleUpdate(uint32_t now) {
  if (!bleScanning) {
    bleRender();
    if (now - bleLastEnd > 800) bleStartScan();
  } else {
    static uint32_t last = 0; static int p = 0;   // barrido "buscando"
    if (now - last > 60) {
      last = now; pixels.clear();
      pixels.setPixelColor(p % NUM_PIXELS, pixels.Color(0, 20, 40));
      pixels.show(); p++;
    }
  }
}

// ======================= Chat de grupo (ESP-NOW) =====================
// Chat grupal entre badges AHAU por ESP-NOW (broadcast). Cada quien se conecta
// al AP de SU badge, escribe en el panel web y el mensaje se difunde por radio
// a TODOS los badges AHAU cercanos que esten en modo AP. Es comunicacion
// CONSENTIDA entre participantes: solo se aceptan tramas con la firma AHAU
// (CHAT_MAGIC) y se ignora cualquier otro trafico; no toca dispositivos ajenos.
//
// Todos los badges fijan su SoftAP al mismo canal (ESPNOW_CHANNEL) para poder
// oirse. ESP-NOW solo vive mientras el AP esta activo (se libera al salir).

static const uint8_t chatBroadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
#define CHAT_MAGIC     0xA4        // firma: "esto es un mensaje de chat AHAU"
#define CHAT_VER       1
#define CHAT_FROM_LEN  24
#define CHAT_TEXT_LEN  180
#define CHAT_MAX_MSGS  12          // historial reciente que se guarda/muestra

struct __attribute__((packed)) ChatPacket {
  uint8_t magic;
  uint8_t ver;
  char    from[CHAT_FROM_LEN];
  char    text[CHAT_TEXT_LEN];
};

struct ChatMsg { char from[CHAT_FROM_LEN]; char text[CHAT_TEXT_LEN]; bool mine; };
ChatMsg  chatLog[CHAT_MAX_MSGS];
volatile int      chatHead  = 0;         // siguiente posicion a escribir (buffer circular)
volatile int      chatCount = 0;         // cuantos mensajes validos hay
volatile uint32_t chatSeq   = 0;         // id incremental (el cliente detecta nuevos)
volatile uint32_t chatNotifyUntil = 0;   // hasta cuando avisar de un mensaje entrante
bool     chatReady = false;

void chatStore(const char* from, const char* text, bool mine) {
  int idx = chatHead;
  strncpy(chatLog[idx].from, from, CHAT_FROM_LEN - 1); chatLog[idx].from[CHAT_FROM_LEN-1] = 0;
  strncpy(chatLog[idx].text, text, CHAT_TEXT_LEN - 1); chatLog[idx].text[CHAT_TEXT_LEN-1] = 0;
  chatLog[idx].mine = mine;
  chatHead = (chatHead + 1) % CHAT_MAX_MSGS;
  if (chatCount < CHAT_MAX_MSGS) chatCount++;
  chatSeq++;
}

// Callback de recepcion ESP-NOW. Corre en la tarea WiFi: mantenerlo breve.
void onChatRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != (int)sizeof(ChatPacket)) return;
  const ChatPacket* p = (const ChatPacket*)data;
  if (p->magic != CHAT_MAGIC || p->ver != CHAT_VER) return;   // no es chat AHAU: ignorar
  char from[CHAT_FROM_LEN]; char text[CHAT_TEXT_LEN];
  memcpy(from, p->from, CHAT_FROM_LEN); from[CHAT_FROM_LEN-1] = 0;
  memcpy(text, p->text, CHAT_TEXT_LEN); text[CHAT_TEXT_LEN-1] = 0;
  chatStore(from, text, false);
  chatNotifyUntil = millis() + 1500;      // aviso suave en el LED integrado
  Serial.printf("[Chat] %s: %s\n", from, text);
}

void chatSend(const String& text) {
  if (!chatReady) return;
  ChatPacket p; memset(&p, 0, sizeof(p));
  p.magic = CHAT_MAGIC; p.ver = CHAT_VER;
  badgeName.toCharArray(p.from, CHAT_FROM_LEN);
  String t = text;
  if (t.length() > CHAT_TEXT_LEN - 1) t = t.substring(0, CHAT_TEXT_LEN - 1);
  t.toCharArray(p.text, CHAT_TEXT_LEN);
  esp_err_t r = esp_now_send(chatBroadcast, (uint8_t*)&p, sizeof(p));
  chatStore(badgeName.c_str(), p.text, true);   // guardar mi propio mensaje (no hay eco de vuelta)
  if (r != ESP_OK) Serial.printf("[Chat] envio fallo: %d\n", (int)r);
}

void chatBegin() {                  // se llama con el SoftAP ya levantado
  if (chatReady) return;
  if (esp_now_init() != ESP_OK) { Serial.println("[Chat] ESP-NOW init fallo"); return; }
  esp_now_register_recv_cb(onChatRecv);
  esp_now_peer_info_t peer; memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, chatBroadcast, 6);
  peer.channel = ESPNOW_CHANNEL;    // mismo canal que el SoftAP (no cambia de canal al enviar)
  peer.ifidx   = WIFI_IF_AP;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(chatBroadcast)) esp_now_add_peer(&peer);
  chatReady = true;
  Serial.printf("[Chat] ESP-NOW listo (grupo AHAU, canal %d)\n", ESPNOW_CHANNEL);
}

void chatEnd() {
  if (!chatReady) return;
  esp_now_unregister_recv_cb();
  if (esp_now_is_peer_exist(chatBroadcast)) esp_now_del_peer(chatBroadcast);
  esp_now_deinit();
  chatReady = false;
}

// ======================= MODO AP + Panel Web ==========================
// El badge levanta su propio SoftAP y sirve un panel de control. Desde ahi
// se cambia de modo, brillo, estilo de arcoiris y se envia un mensaje/flag
// para deletrear en Morse. (No incluye ninguna funcion ofensiva.)
//
// El AP funciona como CONTROL MAESTRO: queda siempre arriba y desde la web se
// elige que muestra la tira (efectos que no tocan el radio) y se disparan
// escaneos WiFi en vivo (AP+STA) sin perder la conexion. El monitor promiscuo
// y el BLE siguen como modos aparte porque toman el radio en exclusiva.

// Que dibuja la tira mientras el AP esta activo (elegible desde la web)
enum { FX_SWEEP = 0, FX_RAINBOW, FX_MORSE, FX_GAME, FX_SOLID, FX_OFF, FX_WIFISCAN, FX_IDENTITY, FX_COUNT };
int     apFx = FX_RAINBOW;   // efecto por defecto del AP: arcoiris
uint8_t solidR = 0, solidG = 0, solidB = 80;
bool    apScanRunning = false;

void apSetFx(int fx);        // declaraciones adelantadas (las usan los handlers)
void apScanStart();

// Nombre unico por defecto, derivado de la MAC (ej. "Badge-3F406B")
String defaultBadgeName() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);   // lee la MAC de efuse (funciona con WiFi apagado)
  char buf[16];
  snprintf(buf, sizeof(buf), "Badge-%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(buf);
}

static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AHAU CTF Badge</title>
<style>
:root{color-scheme:dark}
body{margin:0;font-family:system-ui,sans-serif;background:#0e1116;color:#e6edf3;padding:18px;max-width:520px;margin:auto}
h1{font-size:1.3rem;margin:.2rem 0 1rem}
.card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:14px;margin-bottom:14px}
.card h2{font-size:.85rem;text-transform:uppercase;letter-spacing:.05em;color:#8b949e;margin:0 0 10px}
button{font:inherit;cursor:pointer;border:1px solid #30363d;background:#21262d;color:#e6edf3;border-radius:9px;padding:10px 12px;margin:4px 4px 0 0}
button:active{transform:translateY(1px)}
button.on{border-color:#2f81f7;background:#132a47}
input[type=text]{font:inherit;background:#0e1116;color:#e6edf3;border:1px solid #30363d;border-radius:9px;padding:10px;width:100%;box-sizing:border-box;margin-bottom:8px}
.status{font-size:.8rem;color:#8b949e}
.grid{display:flex;flex-wrap:wrap}
</style></head><body>
<h1>🐬 AHAU CTF Badge — Control</h1>

<div class="card"><h2>Nombre del badge (SSID)</h2>
<div class="status" id="ap" style="margin-bottom:8px">--</div>
<input id="nm" type="text" maxlength="20" placeholder="tu-handle">
<button onclick="setName()">Renombrar AP</button>
<small style="color:#6e7681">El SSID sera AHAU-&lt;nombre&gt;. Al cambiarlo, el AP se reinicia y debes reconectarte.</small>
</div>

<div class="card"><h2>💬 Chat de grupo (ESP-NOW)</h2>
<div id="chat" style="height:180px;overflow-y:auto;background:#0e1116;border:1px solid #30363d;border-radius:9px;padding:8px;margin-bottom:8px;font-size:.9rem">(sin mensajes)</div>
<div style="display:flex;gap:6px">
<input id="msg" type="text" maxlength="160" placeholder="mensaje al grupo AHAU" style="margin-bottom:0" onkeydown="if(event.key==='Enter')chatSend()">
<button onclick="chatSend()" style="margin:0;white-space:nowrap">Enviar</button>
</div>
<small style="color:#6e7681">Se difunde por radio a los badges AHAU cercanos que esten en modo AP.</small>
</div>

<div class="card"><h2>Pantalla del badge</h2><div class="grid" id="fx">
<button onclick="g('/fx?f=7')" data-f="7">Identidad</button>
<button onclick="g('/fx?f=0')" data-f="0">Reposo</button>
<button onclick="g('/fx?f=1')" data-f="1">Arcoiris</button>
<button onclick="g('/fx?f=2')" data-f="2">Morse</button>
<button onclick="g('/fx?f=3')" data-f="3">Juego</button>
<button onclick="g('/fx?f=4')" data-f="4">Color</button>
<button onclick="g('/fx?f=5')" data-f="5">Apagar</button>
<button onclick="g('/fx?f=6')" data-f="6">Esc.WiFi</button>
</div></div>

<div class="card"><h2>Color solido</h2>
<input id="col" type="color" value="#0000ff" style="width:64px;height:40px;background:#0e1116;border:1px solid #30363d;border-radius:9px;vertical-align:middle">
<button onclick="sendColor()">Aplicar</button>
</div>

<div class="card"><h2>Juego · Atrapa la luz</h2>
<button onclick="g('/game')" style="padding:14px 20px;font-size:1.1rem">Jugar / ¡Atrapar!</button>
<span class="status" id="gs"></span>
</div>

<div class="card"><h2>Mensaje en Morse</h2>
<input id="w" type="text" maxlength="40" placeholder="AHAU CTF 2026" value="">
<button onclick="sendMorse()">Deletrear</button>
<button onclick="g('/morse?w=')">Palabras al azar</button>
</div>

<div class="card"><h2>Estilo arcoiris</h2><div class="grid">
<button onclick="g('/rainbow?s=0')">Arcoiris</button>
<button onclick="g('/rainbow?s=1')">Cometa</button>
<button onclick="g('/rainbow?s=2')">Respiracion</button>
</div></div>

<div class="card"><h2>Brillo</h2><div class="grid">
<button onclick="g('/bright?v=40')">Bajo</button>
<button onclick="g('/bright?v=128')">Medio</button>
<button onclick="g('/bright?v=255')">Alto</button>
</div></div>

<div class="card"><h2>Escaner WiFi</h2>
<button onclick="g('/apscan')">Escanear ahora</button>
<div id="aps" class="status" style="margin-top:10px">--</div></div>

<div class="card"><h2>Modos con radio dedicado</h2>
<small style="color:#6e7681">Toman el radio en exclusiva y <b>cierran el AP</b> (perderas esta pagina):</small>
<div class="grid" style="margin-top:8px">
<button onclick="g('/mode?m=1')">Monitor WiFi</button>
<button onclick="g('/mode?m=6')">Escaner BLE</button>
</div>
<div id="bles" class="status" style="margin-top:10px">--</div></div>

<div class="card"><div class="status" id="st">...</div></div>

<script>
function g(u){return fetch(u).then(refresh)}
function sendMorse(){g('/morse?w='+encodeURIComponent(document.getElementById('w').value))}
function sendColor(){var v=document.getElementById('col').value;
  g('/color?r='+parseInt(v.substr(1,2),16)+'&g='+parseInt(v.substr(3,2),16)+'&b='+parseInt(v.substr(5,2),16));}
function setName(){fetch('/name?n='+encodeURIComponent(document.getElementById('nm').value))
  .then(r=>r.text()).then(t=>{alert('Reconectate al nuevo AP:\n'+t);});}
const FXN=['Reposo','Arcoiris','Morse','Juego','Color','Apagado','Esc.WiFi','Identidad'];
function refresh(){return fetch('/status').then(r=>r.json()).then(s=>{
  document.querySelectorAll('#fx button').forEach(b=>b.classList.toggle('on',+b.dataset.f===s.fx));
  document.getElementById('ap').textContent='AP actual: '+s.ssid;
  document.getElementById('gs').textContent=' puntaje: '+s.score;
  document.getElementById('st').textContent=
    'Pantalla: '+FXN[s.fx]+' | paquetes WiFi: '+s.packets+' | juego: '+s.score+
    (s.custom?(' | morse: '+s.custom):'');
})}
function fmt(l){return l.length?l.join('<br>'):'(vacio)';}
function refreshLists(){
  fetch('/scan').then(r=>r.json()).then(a=>{document.getElementById('aps').innerHTML=
    fmt(a.map(x=>(x.open?'🔓':'🔒')+' '+(x.ssid||'(oculta)')+' · ch'+x.ch+' · '+x.rssi+'dBm'));});
  fetch('/blescan').then(r=>r.json()).then(a=>{document.getElementById('bles').innerHTML=
    fmt(a.map(x=>'• '+(x.name||'(sin nombre)')+' · '+x.rssi+'dBm'));});
}
function chatSend(){var i=document.getElementById('msg');var t=i.value.trim();
  if(!t)return; i.value='';
  fetch('/chat/send?m='+encodeURIComponent(t)).then(chatRefresh);}
function chatRefresh(){return fetch('/chat/log').then(r=>r.json()).then(s=>{
  var box=document.getElementById('chat'); box.innerHTML='';
  if(!s.msgs.length){var e=document.createElement('div');e.style.color='#6e7681';e.textContent='(sin mensajes)';box.appendChild(e);return;}
  s.msgs.forEach(m=>{
    var row=document.createElement('div'); row.style.margin='2px 0';
    var who=document.createElement('b'); who.textContent=(m.mine?'yo':m.from)+': ';
    who.style.color=m.mine?'#2f81f7':'#3fb950';
    var tx=document.createElement('span'); tx.textContent=m.text;   // textContent: evita inyeccion HTML
    row.appendChild(who); row.appendChild(tx); box.appendChild(row);
  });
  box.scrollTop=box.scrollHeight;
});}
refresh();refreshLists();chatRefresh();
setInterval(()=>{refresh();refreshLists();},2000);
setInterval(chatRefresh,1500);
</script>
</body></html>)HTML";

void handleRoot()   { server.send_P(200, "text/html", PAGE_HTML); }

void handleMode() {
  if (server.hasArg("m")) {
    int m = server.arg("m").toInt();
    if (m >= 0 && m < MODE_COUNT) pendingMode = m;   // se aplica en loop()
  }
  server.send(200, "text/plain", "ok");
}

void handleBright() {
  if (server.hasArg("v")) pixels.setBrightness(constrain(server.arg("v").toInt(), 1, 255));
  server.send(200, "text/plain", "ok");
}

void handleRainbow() {
  if (server.hasArg("s")) rbStyle = constrain(server.arg("s").toInt(), 0, 2);
  server.send(200, "text/plain", "ok");
}

void handleMorse() {
  if (server.hasArg("w")) {
    String w = server.arg("w"); w.toUpperCase();
    String clean = "";
    for (uint16_t i = 0; i < w.length() && clean.length() < 40; i++) {
      char c = w.charAt(i);
      if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') clean += c;
    }
    customWord = clean;
    if (currentMode == MODE_MORSE) elegirPalabraAlAzar();
  }
  server.send(200, "text/plain", "ok");
}

void handleStatus() {
  String j = "{\"mode\":" + String(currentMode) +
             ",\"fx\":" + String(apFx) +
             ",\"ssid\":\"" + apSSID + "\"" +
             ",\"packets\":" + String(totalPackets) +
             ",\"score\":" + String(gScore) +
             ",\"custom\":\"" + customWord + "\"}";
  server.send(200, "application/json", j);
}

void handleName() {                  // renombra el AP (SSID) y lo persiste
  if (server.hasArg("n")) {
    String n = server.arg("n");
    String clean = "";
    for (uint16_t i = 0; i < n.length() && clean.length() < 20; i++) {
      char c = n.charAt(i);
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_') clean += c;
    }
    if (clean.length() == 0) clean = defaultBadgeName();
    badgeName = clean;
    apSSID = String(AP_PREFIX) + badgeName;
    computeSignature();              // la firma visual cambia con el nombre
    prefs.begin("badge", false);
    prefs.putString("name", badgeName);
    prefs.end();
    pendingApRestart = true;         // el AP se reinicia en loop() con el nuevo SSID
  }
  server.send(200, "text/plain", apSSID);
}

void handleFx() {                    // elige que dibuja la tira (sin salir del AP)
  if (server.hasArg("f")) {
    int f = server.arg("f").toInt();
    if (f >= 0 && f < FX_COUNT) apSetFx(f);
  }
  server.send(200, "text/plain", "ok");
}

void handleColor() {                 // color solido
  if (server.hasArg("r")) solidR = constrain(server.arg("r").toInt(), 0, 255);
  if (server.hasArg("g")) solidG = constrain(server.arg("g").toInt(), 0, 255);
  if (server.hasArg("b")) solidB = constrain(server.arg("b").toInt(), 0, 255);
  apSetFx(FX_SOLID);
  server.send(200, "text/plain", "ok");
}

void handleGame() {                  // jugar / atrapar desde la web
  if (apFx != FX_GAME) apSetFx(FX_GAME);
  gameAction();
  server.send(200, "text/plain", "ok");
}

void handleApScan() {                // escaneo WiFi en vivo, sin cerrar el AP
  if (apFx != FX_WIFISCAN) apSetFx(FX_WIFISCAN);
  else apScanStart();
  server.send(200, "text/plain", "ok");
}

// Escapa comillas/backslash para meter texto en JSON
String jsonEscape(const char* s) {
  String r = s;
  r.replace("\\", "\\\\");
  r.replace("\"", "\\\"");
  return r;
}

void handleChatSend() {              // enviar un mensaje al grupo por ESP-NOW
  if (server.hasArg("m")) {
    String m = server.arg("m"); m.trim();
    if (m.length() > 0) chatSend(m);
  }
  server.send(200, "text/plain", "ok");
}

void handleChatLog() {               // historial reciente (orden cronologico) en JSON
  String j = "{\"seq\":" + String((uint32_t)chatSeq) + ",\"msgs\":[";
  int start = (chatCount < CHAT_MAX_MSGS) ? 0 : chatHead;
  for (int k = 0; k < chatCount; k++) {
    int idx = (start + k) % CHAT_MAX_MSGS;
    if (k) j += ",";
    j += "{\"from\":\"" + jsonEscape(chatLog[idx].from) +
         "\",\"text\":\"" + jsonEscape(chatLog[idx].text) +
         "\",\"mine\":" + (chatLog[idx].mine ? "true" : "false") + "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleScan() {                  // ultimas redes WiFi capturadas
  String j = "[";
  for (int i = 0; i < apCount; i++) {
    if (i) j += ",";
    j += "{\"ssid\":\"" + jsonEscape(apList[i].ssid) + "\",\"rssi\":" +
         String(apList[i].rssi) + ",\"ch\":" + String(apList[i].ch) +
         ",\"open\":" + (apList[i].open ? "true" : "false") + "}";
  }
  j += "]";
  server.send(200, "application/json", j);
}

void handleBleScan() {               // ultimos dispositivos BLE capturados
  String j = "[";
  int c = bleCount;
  for (int i = 0; i < c; i++) {
    if (i) j += ",";
    j += "{\"name\":\"" + jsonEscape(bleList[i].name) + "\",\"rssi\":" +
         String(bleList[i].rssi) + "}";
  }
  j += "]";
  server.send(200, "application/json", j);
}

void setupWebRoutes() {
  server.on("/", handleRoot);
  server.on("/mode", handleMode);
  server.on("/bright", handleBright);
  server.on("/rainbow", handleRainbow);
  server.on("/morse", handleMorse);
  server.on("/status", handleStatus);
  server.on("/scan", handleScan);
  server.on("/blescan", handleBleScan);
  server.on("/fx", handleFx);
  server.on("/color", handleColor);
  server.on("/game", handleGame);
  server.on("/apscan", handleApScan);
  server.on("/name", handleName);
  server.on("/chat/send", handleChatSend);
  server.on("/chat/log", handleChatLog);
  server.onNotFound(handleRoot);     // cualquier ruta cae al panel
}

void apEnter() {
  esp_wifi_set_promiscuous(false);   // el AP y el modo promiscuo no conviven
  WiFi.mode(WIFI_AP_STA);            // AP + STA: permite escanear sin caer el AP
  // Canal fijo (ESPNOW_CHANNEL) para que el chat ESP-NOW se oiga entre badges.
  WiFi.softAP(apSSID.c_str(), AP_PASS, ESPNOW_CHANNEL);
  setupWebRoutes();
  server.begin();
  chatBegin();                       // chat de grupo ESP-NOW (con el AP ya arriba)
  apSetFx(FX_RAINBOW);               // al abrir el AP la tira muestra arcoiris
  apScanRunning = false;
  Serial.print("[AP] SSID: "); Serial.print(apSSID);
  Serial.print("  IP: ");       Serial.println(WiFi.softAPIP());
}

void apExit() {
  chatEnd();                         // liberar ESP-NOW antes de apagar el radio
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

void apScanStart() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true);           // asincrono; en AP+STA no tumba el AP
  apScanRunning = true;
}

// Elige el efecto que dibuja la tira y lo inicializa (sin salir del AP)
void apSetFx(int fx) {
  apFx = fx;
  pixels.clear();
  switch (fx) {
    case FX_RAINBOW:  rainbowEnter(); break;
    case FX_MORSE:    morseEnter();   break;
    case FX_GAME:     gameEnter();    break;
    case FX_WIFISCAN: apScanStart();  break;
    case FX_SOLID:
      for (int i = 0; i < NUM_PIXELS; i++)
        pixels.setPixelColor(i, pixels.Color(solidR, solidG, solidB));
      break;
  }
  pixels.show();
}

void apAction() {                    // ACCION fisica mientras el AP esta activo
  if (apFx == FX_GAME)       gameAction();                 // atrapar
  else if (apFx == FX_MORSE) morseAction();                // otra palabra
  else                       apSetFx((apFx + 1) % FX_COUNT); // cambia lo mostrado
}

void apUpdate(uint32_t now) {
  server.handleClient();

  // Aviso suave de mensaje de chat entrante: el LED integrado respira en verde
  // ~1.5 s (sin parpadeo brusco); luego vuelve al color del modo.
  static bool chatNotifyActive = false;
  if (now < chatNotifyUntil) {
    float lv = 0.5 - 0.5 * cosf(now * 0.012);
    onboard.setPixelColor(0, onboard.Color(0, (uint8_t)(50 * lv) + 6, 0));
    onboard.show();
    chatNotifyActive = true;
  } else if (chatNotifyActive) {
    showOnboard(MODE_AP);
    chatNotifyActive = false;
  }

  switch (apFx) {
    case FX_RAINBOW:  rainbowUpdate(now);  break;
    case FX_MORSE:    morseUpdate(now);    break;
    case FX_GAME:     gameUpdate(now);     break;
    case FX_IDENTITY: identityUpdate(now); break;
    case FX_SOLID:                         // ya dibujado en apSetFx/handleColor
    case FX_OFF:      break;               // nada que animar
    case FX_WIFISCAN: {
      int n = WiFi.scanComplete();
      if (n >= 0) {                        // escaneo listo
        apCount = (n > NUM_PIXELS) ? NUM_PIXELS : n;
        for (int i = 0; i < apCount; i++) {
          WiFi.SSID(i).toCharArray(apList[i].ssid, sizeof(apList[i].ssid));
          apList[i].rssi = WiFi.RSSI(i);
          apList[i].ch   = WiFi.channel(i);
          apList[i].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        }
        WiFi.scanDelete();
        apScanRunning = false;
        scanRender();
      } else if (apScanRunning) {          // barrido "buscando"
        static uint32_t last = 0; static int p = 0;
        if (now - last > 60) {
          last = now; pixels.clear();
          pixels.setPixelColor(p % NUM_PIXELS, pixels.Color(20, 20, 20));
          pixels.show(); p++;
        }
      }
      break;
    }
    default: {                             // FX_SWEEP: barrido azul "AP activo"
      static uint32_t last = 0; static int p = 0, d = 1;
      if (now - last >= 70) {
        last = now;
        for (int i = 0; i < NUM_PIXELS; i++) {
          uint8_t b = pixels.getPixelColor(i) & 0xFF;
          pixels.setPixelColor(i, pixels.Color(0, 0, b * 2 / 3));
        }
        pixels.setPixelColor(p, pixels.Color(0, 0, 160));
        p += d;
        if (p >= NUM_PIXELS - 1 || p <= 0) d = -d;
        pixels.show();
      }
      break;
    }
  }
}

// ======================= Despacho de modos ============================
void exitMode(int m) {
  switch (m) {                       // limpieza al abandonar un modo
    case MODE_WIFI:     esp_wifi_set_promiscuous(false); WiFi.mode(WIFI_OFF); break;
    case MODE_WIFISCAN: scanExit();  WiFi.mode(WIFI_OFF); break;
    case MODE_BLESCAN:  bleExit();   break;   // bleExit ya apaga el WiFi
    case MODE_AP:       apExit();    break;   // apExit ya apaga el WiFi
  }
}

// Color representativo de cada modo (para la etiqueta en la tira)
uint32_t modeTagColor(int m) {
  switch (m) {
    case MODE_WIFI:     return pixels.Color(0, 0, 120);
    case MODE_RAINBOW:  return pixels.Color(90, 0, 90);
    case MODE_MORSE:    return pixels.Color(0, 100, 100);
    case MODE_GAME:     return pixels.Color(120, 60, 0);
    case MODE_WIFISCAN: return pixels.Color(0, 120, 0);
    case MODE_BLESCAN:  return pixels.Color(0, 60, 120);
    case MODE_AP:       return pixels.Color(110, 110, 110);
    case MODE_IDENTITY: return Wheel(sigHue1);
  }
  return pixels.Color(60, 60, 60);
}

// Indica el modo de forma clara y ESTABLE: enciende (m+1) LEDs fijos del color
// del modo por ~0.6 s antes de arrancarlo. Asi siempre sabes en que modo estas.
void showModeTag(int m) {
  uint32_t c = modeTagColor(m);
  int n = m + 1; if (n > NUM_PIXELS) n = NUM_PIXELS;
  pixels.clear();
  for (int i = 0; i < n; i++) pixels.setPixelColor(i, c);
  pixels.show();
  delay(600);
  pixels.clear(); pixels.show();
}

void enterMode(int m) {
  exitMode(currentMode);
  currentMode = m;
  pixels.clear(); pixels.show();
  showOnboard(m);
  showModeTag(m);                    // etiqueta visual clara del modo
  switch (m) {
    case MODE_WIFI:     wifiEnter();    break;
    case MODE_RAINBOW:  rainbowEnter(); break;
    case MODE_MORSE:    morseEnter();   break;
    case MODE_GAME:     gameEnter();    break;
    case MODE_WIFISCAN: scanEnter();    break;
    case MODE_BLESCAN:  bleEnter();     break;
    case MODE_AP:       apEnter();      break;
    case MODE_IDENTITY: break;          // la firma se dibuja en identityUpdate()
  }
  Serial.printf("== Modo: %d ==\n", m);
}

// ======================= setup / loop =================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // serial NO bloqueante: si nadie lee el USB, no congela el loop
  delay(300);

  pixels.begin();
  pixels.setBrightness(brightnessLevels[brightnessIndex]);
  pixels.clear(); pixels.show();

  onboard.begin();
  onboard.setBrightness(60);
  onboard.clear(); onboard.show();

  buttonInit(btnMode, BTN_MODE_PIN);
  buttonInit(btnAction, BTN_ACTION_PIN);

  randomSeed(esp_random());

  // WiFi en modo promiscuo (para el modo Monitor). Queda activo siempre;
  // solo se dibuja cuando estamos en MODE_WIFI.
  // Radio apagado por defecto. Se enciende solo en los modos que lo usan
  // (WiFi/Escaner/AP); asi la tira no destella por interferencia de RMT+WiFi
  // y el badge es mas estable.
  WiFi.mode(WIFI_OFF);

  // Nombre unico por badge (persistente en NVS). Por defecto deriva de la MAC;
  // se puede renombrar desde el panel web para reconocerlo facil.
  prefs.begin("badge", true);
  badgeName = prefs.getString("name", "");
  prefs.end();
  if (badgeName.length() == 0) badgeName = defaultBadgeName();
  apSSID = String(AP_PREFIX) + badgeName;
  computeSignature();               // color/patron unico derivado del nombre
  Serial.print("[Badge] AP: "); Serial.println(apSSID);

  enterMode(currentMode);
}

void loop() {
  uint32_t now = millis();

  buttonUpdate(btnMode, now);
  buttonUpdate(btnAction, now);

  // MODO: pulsacion larga cambia brillo; corta (al soltar) cambia de modo
  if (btnMode.longEvent) {
    brightnessIndex = (brightnessIndex + 1) % (sizeof(brightnessLevels));
    pixels.setBrightness(brightnessLevels[brightnessIndex]);
    Serial.printf("[Brillo] nivel %d\n", brightnessLevels[brightnessIndex]);
  }
  if (btnMode.rose && !btnMode.longFired) {
    enterMode((currentMode + 1) % MODE_COUNT);
  }

  // ACCION: funcion del modo actual (al presionar, para respuesta rapida)
  if (btnAction.fell) {
    switch (currentMode) {
      case MODE_WIFI:     wifiAction();    break;
      case MODE_RAINBOW:  rainbowAction(); break;
      case MODE_MORSE:    morseAction();   break;
      case MODE_GAME:     gameAction();    break;
      case MODE_WIFISCAN: scanAction();    break;
      case MODE_BLESCAN:  bleAction();     break;
      case MODE_AP:       apAction();      break;
    }
  }

  // Actualizar el modo activo
  switch (currentMode) {
    case MODE_WIFI:     wifiUpdate(now);    break;
    case MODE_RAINBOW:  rainbowUpdate(now); break;
    case MODE_MORSE:    morseUpdate(now);   break;
    case MODE_GAME:     gameUpdate(now);    break;
    case MODE_WIFISCAN: scanUpdate(now);    break;
    case MODE_BLESCAN:  bleUpdate(now);     break;
    case MODE_AP:       apUpdate(now);      break;
    case MODE_IDENTITY: identityUpdate(now); break;
  }

  // Cambio de modo pedido desde la web (aplicado aqui, fuera del handler,
  // para poder desmontar el AP con seguridad)
  if (pendingMode >= 0) {
    int m = pendingMode;
    pendingMode = -1;
    enterMode(m);
  }

  // Reinicio del AP con nuevo SSID (fuera del handler para no cortar la
  // respuesta HTTP; el cliente debe reconectarse al nuevo nombre)
  if (pendingApRestart && currentMode == MODE_AP) {
    pendingApRestart = false;
    WiFi.softAPdisconnect(false);
    WiFi.softAP(apSSID.c_str(), AP_PASS, ESPNOW_CHANNEL);   // mismo canal: no rompe el chat
    Serial.print("[AP] nuevo SSID: "); Serial.println(apSSID);
  }

  delay(4);   // cede CPU a las tareas de fondo (WiFi/BLE) y alimenta el watchdog
}
