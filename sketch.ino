/**
 * ============================================================================
 *  CONTROL DE ACCESO SEGURO CON REGISTRO DE EVENTOS
 * ============================================================================
 *  Plataforma : ESP32 DevKit V1 (Arduino framework)
 *  Entorno    : Arduino IDE / PlatformIO / Wokwi
 *  Librerías  : Keypad, ESP32Servo
 *
 *  Descripción:
 *    Sistema de autenticación por PIN activado por un sensor PIR. Controla una
 *    cerradura simulada (servo SG90) y señaliza con LEDs. Cada evento relevante
 *    se registra en la nube (Google Sheets vía Apps Script, HTTP GET sobre TLS).
 *
 *  Flujo (máquina de estados):
 *    ESPERA -> ESPERANDO_PIN -> ABIERTO ----------> ESPERA
 *                            -> BLOQUEADO (60 s) -> ESPERA
 *
 *  Telemetría enviada a la nube:
 *    - BOOT_TELEMETRY : primera conexión WiFi (IP, RSSI, heap, reinicio)
 *    - HEARTBEAT      : latido periódico (estado de salud del dispositivo)
 *    - Eventos de acceso: MOTION_DETECTED, ACCESS_GRANTED, ACCESS_DENIED,
 *                         LOCKOUT, LOCKOUT_END, PIN_TIMEOUT, DOOR_LOCKED
 * ============================================================================
 */
#include <Arduino.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Keypad.h>
#include <ESP32Servo.h>

// ============================ CONFIGURACIÓN =================================

// --- Seguridad de acceso ---
const String PIN_CORRECTO = "1234";            // PIN principal (usuario 1)

// Usuarios autorizados: cada PIN identifica a una persona en el registro de la nube.
struct Usuario { const char* pin; const char* id; };
const Usuario USUARIOS[] = {
  { PIN_CORRECTO.c_str(), "USR-001-ADMIN" },
  { "5678",               "USR-002-JUAN"  },
  { "9012",               "USR-003-MARIA" }
};
const int NUM_USUARIOS = sizeof(USUARIOS) / sizeof(USUARIOS[0]);
const int    MAX_INTENTOS = 3;                 // fallos consecutivos antes de bloquear

// --- Temporizaciones (milisegundos) ---
const unsigned long TIMEOUT_PIN_MS  = 15000;   // inactividad máxima esperando PIN
const unsigned long BLOQUEO_MS      = 60000;   // duración del bloqueo temporal
const unsigned long APERTURA_MS     = 5000;    // tiempo con la cerradura abierta
const unsigned long LED_VERDE_MS    = 3000;    // LED verde encendido tras acceso
const unsigned long PARPADEO_MS     = 300;     // período de intermitencia del LED rojo
const unsigned long HEARTBEAT_MS    = 300000;  // latido de telemetría (5 min)
const unsigned long REINTENTO_MS    = 2000;    // reintento de envío de la cola
const unsigned long WIFI_REINTENTO  = 10000;   // reintento de reconexión WiFi

// --- Servo (ángulos) ---
const int SERVO_CERRADO = 0;
const int SERVO_ABIERTO = 90;

// --- Red y nube ---
const char* WIFI_SSID = "Wokwi-GUEST";         // red virtual de Wokwi
const char* WIFI_PASS = "";                    // sin contraseña en Wokwi
const int   WIFI_CANAL = 6;                    // acelera la conexión en Wokwi
// URL de la Web App de Apps Script (termina en /exec)
const String WEBHOOK_URL = "https://script.google.com/macros/s/TU_ID_DE_DESPLIEGUE/exec";
const String API_TOKEN   = "CAMBIA_ESTE_TOKEN"; // debe coincidir con el del Apps Script
const String DEVICE_ID   = "ESP32-PUERTA-01";

// ================================ PINES =====================================
const int PIN_PIR       = 27;   // salida del sensor PIR HC-SR501
const int PIN_SERVO     = 13;   // señal PWM del servo SG90
const int PIN_LED_VERDE = 25;   // acceso autorizado
const int PIN_LED_ROJO  = 26;   // acceso denegado / bloqueo

// Teclado matricial 4x4
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {19, 18, 5, 23};
byte colPins[COLS] = {32, 33, 14, 4};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

Servo cerradura;

// ============================ ESTADO DEL SISTEMA ============================
enum Estado { ESPERA, ESPERANDO_PIN, ABIERTO, BLOQUEADO };
Estado estado = ESPERA;

String pinIngresado = "";          // PIN en construcción (nunca se envía a la nube)
int intentos = 0;                  // fallos consecutivos
String usuarioActual = "";         // usuario con la puerta abierta (para DOOR_LOCKED)
unsigned long tEstado = 0;         // instante de entrada al estado actual
unsigned long tUltimaTecla = 0;    // última pulsación (para el timeout)

// Banderas gestionadas desde el callback de WiFi (otra tarea) -> volatile
volatile bool wifiRecienConectado = false;
bool bootTelemetryEnviada = false;

// ======================= COLA DE EVENTOS (BUFFER) ===========================
// Si no hay red, los eventos se conservan y se reenvían: no se pierde trazabilidad.
struct Evento { String tipo; String detalle; unsigned long ms; String usuario; time_t ts; };
const int COLA_MAX = 20;
Evento cola[COLA_MAX];
int colaHead = 0, colaCount = 0;
SemaphoreHandle_t colaMutex;   // protege la cola (la comparten loop() y la tarea de red)

/** Registra el evento en consola y lo encola para su envío a la nube. */
void encolarEvento(const String& tipo, const String& detalle = "", const String& usuario = "") {
  Serial.printf("[%7lu ms][EVENTO] %s %s %s\n", millis(), tipo.c_str(),
                usuario.c_str(), detalle.c_str());
  xSemaphoreTake(colaMutex, portMAX_DELAY);
  if (colaCount == COLA_MAX) {                 // cola llena: descarta el más antiguo
    colaHead = (colaHead + 1) % COLA_MAX;
    colaCount--;
  }
  cola[(colaHead + colaCount) % COLA_MAX] = {tipo, detalle, millis(), usuario, time(nullptr)};
  colaCount++;
  xSemaphoreGive(colaMutex);
}

/** Codifica texto para usarlo dentro de una URL. */
String urlEncode(const String& s) {
  String r; char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.') r += c;
    else { snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c); r += buf; }
  }
  return r;
}

/** Marca de tiempo ISO-8601 (UTC) tomada por el ESP32. Vacía si NTP aún no sincroniza. */
String marcaTiempo(time_t t) {
  if (t < 1700000000) return "";            // hora no sincronizada todavía
  struct tm info; gmtime_r(&t, &info);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &info);
  return String(buf);
}

/** Envía un evento al webhook. Devuelve true si Google respondió 200. */
bool enviarEvento(const Evento& e) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();   // DEMO: no valida certificado. En producción, fijar CA.
  HTTPClient http;

  String url = WEBHOOK_URL
             + "?token="  + urlEncode(API_TOKEN)
             + "&device=" + urlEncode(DEVICE_ID)
             + "&event="  + urlEncode(e.tipo)
             + "&user="   + urlEncode(e.usuario)
             + "&ts="     + urlEncode(marcaTiempo(e.ts))
             + "&detail=" + urlEncode(e.detalle)
             + "&uptime=" + String(e.ms / 1000)
             + "&rssi="   + String(WiFi.RSSI());

  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // Apps Script responde 302
  http.setTimeout(8000);
  int code = http.GET();
  http.end();

  Serial.printf("[%7lu ms][HTTP] %s -> codigo %d\n", millis(), e.tipo.c_str(), code);
  return code == 200;
}

/**
 * Tarea de red (corre en el núcleo 0, separada de loop()).
 * Las peticiones HTTPS pueden tardar varios segundos; al hacerlas aquí,
 * el teclado, el servo y los LEDs nunca quedan bloqueados.
 */
void tareaEnvio(void* param) {
  for (;;) {
    Evento e;
    bool hay = false;

    xSemaphoreTake(colaMutex, portMAX_DELAY);
    if (colaCount > 0) { e = cola[colaHead]; hay = true; }   // copia, sin quitarlo aún
    xSemaphoreGive(colaMutex);

    if (hay && enviarEvento(e)) {                            // envío fuera del mutex
      xSemaphoreTake(colaMutex, portMAX_DELAY);
      if (colaCount > 0) { colaHead = (colaHead + 1) % COLA_MAX; colaCount--; }
      xSemaphoreGive(colaMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(hay ? 200 : REINTENTO_MS));
  }
}

// ================================= WIFI =====================================
/** Callback de eventos WiFi: deja evidencia de la conexión en consola. */
void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.printf("[%7lu ms][WiFi] Iniciando conexion a '%s'...\n", millis(), WIFI_SSID);
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("[%7lu ms][WiFi] Asociado al punto de acceso\n", millis());
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[%7lu ms][WiFi] CONECTADO | IP=%s | GW=%s | RSSI=%d dBm\n",
                    millis(), WiFi.localIP().toString().c_str(),
                    WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
      wifiRecienConectado = true;   // el loop() enviará la telemetría inicial
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[%7lu ms][WiFi] Desconectado\n", millis());
      break;
    default: break;
  }
}

/** Reintenta la conexión si se perdió (cada WIFI_REINTENTO ms). */
void asegurarWiFi() {
  static unsigned long ultimo = 0;
  if (WiFi.status() == WL_CONNECTED || millis() - ultimo < WIFI_REINTENTO) return;
  ultimo = millis();
  Serial.printf("[%7lu ms][WiFi] Reintentando conexion...\n", millis());
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CANAL);
}

// ============================ TELEMETRÍA ====================================
/** Detalle de salud del dispositivo (IP, señal, memoria libre). */
String resumenTelemetria() {
  return "ip=" + WiFi.localIP().toString()
       + ";rssi=" + String(WiFi.RSSI())
       + ";heap=" + String(ESP.getFreeHeap())
       + ";intentos=" + String(intentos);
}

/** Telemetría inicial (una vez) y latido periódico. */
void gestionarTelemetria() {
  static unsigned long ultimoLatido = 0;

  if (wifiRecienConectado && !bootTelemetryEnviada) {
    bootTelemetryEnviada = true;
    wifiRecienConectado = false;
    encolarEvento("BOOT_TELEMETRY", "fw=v1.0;" + resumenTelemetria());
    ultimoLatido = millis();
  }
  if (bootTelemetryEnviada && millis() - ultimoLatido >= HEARTBEAT_MS) {
    ultimoLatido = millis();
    encolarEvento("HEARTBEAT", resumenTelemetria());
  }
}

// ============================ UTILIDADES DE HARDWARE ========================
void leds(bool verde, bool rojo) {
  digitalWrite(PIN_LED_VERDE, verde);
  digitalWrite(PIN_LED_ROJO, rojo);
}

void cambiarEstado(Estado nuevo) {
  estado = nuevo;
  tEstado = millis();
}

/** Estado seguro: puerta cerrada, LEDs apagados, contador de intentos a cero. */
void volverAEspera() {
  pinIngresado = "";
  intentos = 0;
  cerradura.write(SERVO_CERRADO);
  leds(false, false);
  cambiarEstado(ESPERA);
  Serial.println("\n[Sistema en espera]");
}

/** Compara el PIN ingresado y ejecuta la acción correspondiente. */
void validarPin() {
  // Busca el PIN en la tabla de usuarios
  const char* idUsuario = nullptr;
  for (int i = 0; i < NUM_USUARIOS; i++) {
    if (pinIngresado == USUARIOS[i].pin) { idUsuario = USUARIOS[i].id; break; }
  }

  if (idUsuario != nullptr) {
    // --- PIN CORRECTO: abre cerradura, LED verde, registra acceso con el ID del usuario ---
    Serial.printf("\nAcceso autorizado: %s\n", idUsuario);
    usuarioActual = idUsuario;
    encolarEvento("ACCESS_GRANTED", "intento=" + String(intentos + 1), idUsuario);
    cerradura.write(SERVO_ABIERTO);
    leds(true, false);
    intentos = 0;
    cambiarEstado(ABIERTO);
  } else {
    // --- PIN INCORRECTO: LED rojo, suma intento, registra fallo ---
    intentos++;
    Serial.printf("\nPIN incorrecto (%d/%d)\n", intentos, MAX_INTENTOS);
    encolarEvento("ACCESS_DENIED", "intento=" + String(intentos), "DESCONOCIDO");
    leds(false, true);
    delay(800);                       // parpadeo breve de señalización
    leds(false, false);
    pinIngresado = "";

    if (intentos >= MAX_INTENTOS) {
      // --- BLOQUEO TEMPORAL tras 3 fallos consecutivos ---
      encolarEvento("LOCKOUT", "bloqueo " + String(BLOQUEO_MS / 1000) + "s");
      Serial.println("Demasiados intentos. Sistema bloqueado.");
      cambiarEstado(BLOQUEADO);
    } else {
      Serial.println("Ingrese PIN");
      tUltimaTecla = millis();
    }
  }
}

// ================================ SETUP =====================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Control de Acceso Seguro v1.0 ===");
  Serial.printf("Motivo de reinicio: %d | Heap libre: %u bytes\n",
                (int)esp_reset_reason(), ESP.getFreeHeap());

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);

  cerradura.setPeriodHertz(50);
  cerradura.attach(PIN_SERVO, 500, 2400);
  cerradura.write(SERVO_CERRADO);     // arranca siempre cerrado
  leds(false, false);

  // Hora real por NTP (UTC). Los eventos se sellan en el dispositivo al ocurrir,
  // aunque se envíen después por una caída de red.
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  colaMutex = xSemaphoreCreateMutex();   // debe existir antes de encolar eventos

  WiFi.onEvent(onWiFiEvent);          // logs de conexión
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CANAL);

  // Tarea de envío a la nube (pila grande: TLS consume memoria)
  xTaskCreatePinnedToCore(tareaEnvio, "envio", 12288, NULL, 1, NULL, 0);

  Serial.println("Sistema iniciado. En espera...");
}

// ================================ LOOP ======================================
void loop() {
  asegurarWiFi();          // mantiene la red
  gestionarTelemetria();   // telemetría inicial + latido (el envío lo hace tareaEnvio)

  switch (estado) {

    // ---- Espera: el PIR activa el sistema ----
    case ESPERA:
      if (digitalRead(PIN_PIR) == HIGH) {
        encolarEvento("MOTION_DETECTED");
        Serial.println("\nMovimiento detectado\nIngrese PIN");
        pinIngresado = "";
        tUltimaTecla = millis();
        cambiarEstado(ESPERANDO_PIN);
      }
      break;

    // ---- Captura del PIN: # confirma, * borra ----
    case ESPERANDO_PIN: {
      char k = keypad.getKey();
      if (k) {
        tUltimaTecla = millis();
        if (k == '*')       { pinIngresado = ""; Serial.println("\n[PIN borrado]"); }
        else if (k == '#')  { validarPin(); }
        else if (isDigit(k) && pinIngresado.length() < 8) {
          pinIngresado += k;
          Serial.print('*');           // enmascara el PIN en consola
        }
      }
      if (millis() - tUltimaTecla > TIMEOUT_PIN_MS) {
        encolarEvento("PIN_TIMEOUT");
        Serial.println("\nTiempo agotado");
        volverAEspera();
      }
      break;
    }

    // ---- Cerradura abierta: LED verde 3 s, cierre a los 5 s ----
    case ABIERTO:
      if (millis() - tEstado >= LED_VERDE_MS) digitalWrite(PIN_LED_VERDE, LOW);
      if (millis() - tEstado >= APERTURA_MS) {
        encolarEvento("DOOR_LOCKED", "", usuarioActual);
        volverAEspera();
      }
      break;

    // ---- Bloqueo: teclado deshabilitado, LED rojo intermitente ----
    case BLOQUEADO:
      keypad.getKey();                 // descarta pulsaciones
      digitalWrite(PIN_LED_ROJO, ((millis() - tEstado) / PARPADEO_MS) % 2 == 0);
      if (millis() - tEstado >= BLOQUEO_MS) {
        encolarEvento("LOCKOUT_END", "contador reiniciado");
        volverAEspera();
      }
      break;
  }
}
