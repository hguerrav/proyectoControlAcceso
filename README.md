# 🔐 Sistema de Control de Acceso Inteligente con ESP32

Sistema embebido de seguridad física y control de acceso desarrollado sobre **ESP32** (simulado en **Wokwi**), que integra detección de presencia por infrarrojos (PIR), autenticación mediante teclado matricial, actuación electromecánica y telemetría de eventos en tiempo real a través de un servidor HTTP integrado.

---

## 📋 Características Principales

* **Activación Bajo Demanda:** El sistema permanece en reposo hasta que el sensor PIR detecta movimiento (`MOTION_DETECTED`), optimizando el consumo y evitando lecturas accidentales.
* **Autenticación Multiusuario:** Validación de credenciales mediante PIN de 4 dígitos oculto en consola (`****`), con trazabilidad individual por matrícula de usuario.
* **Seguridad Activa Anti-Intrusión:** Bloqueo automático del teclado tras **3 intentos fallidos consecutivos**, acompañado de una alerta visual intermitente.
* **Actuación Automatizada:** Apertura de cerradura mediante servomotor ($90^\circ$) durante 4 segundos y autocierre automático ($0^\circ$).
* **Resiliencia de Red y Telemetría HTTP:** Servidor web integrado en el ESP32 para exponer el registro de auditoría en tiempo real, con rutina de reconexión automática ante caídas de WiFi sin comprometer la operación local.

---

## 🛠️ Arquitectura de Hardware

El prototipo está compuesto por cuatro periféricos principales conectados al microcontrolador **ESP32**:

| Componente | Función en el Sistema | Comportamiento |
| :--- | :--- | :--- |
| **Sensor PIR** | Detección de presencia física | Dispara el evento `MOTION_DETECTED` y habilita la captura del teclado. |
| **Teclado Matricial 4x4** | Interfaz de ingreso de credenciales | Captura el PIN de 4 dígitos con enmascaramiento en salida serial. |
| **Servomotor** | Actuador de cerradura | Gira a $90^\circ$ al conceder acceso y retorna a $0^\circ$ tras el pulso de apertura. |
| **LEDs Indicadores (Verde / Rojo)** | Retroalimentación visual de estado | **Verde:** Acceso autorizado.<br>**Rojo:** Error de PIN o parpadeo de bloqueo activo. |

---

## ⚙️ Parámetros de Temporización del Firmware

Las constantes críticas definidas en el código fuente (`esp32-http-server.ino`) regulan los tiempos de respuesta y seguridad del sistema:

```cpp
const unsigned long TIMEOUT_PIN_MS     = 15000; // Inactividad máxima esperando PIN (15 s)
const unsigned long BLOQUEO_MS         = 30000; // Duración del bloqueo temporal (30 s)
const unsigned long TIEMPO_APERTURA_MS = 4000;  // Pulso de apertura de puerta (4 s)
const unsigned long BLINK_ROJO_MS      = 300;   // Periodo de intermitencia del LED rojo (300 ms)
const unsigned long WIFI_REINTENTO_MS  = 10000; // Intervalo de reconexión WiFi (10 s)
const int MAX_INTENTOS                 = 3;     // Fallos consecutivos antes de bloquear
```

---

## 👥 Perfiles Autorizados (Configuración de Prueba)

El sistema asocia cada PIN válido a un identificador único para auditoría de accesos:

| ID de Usuario | Rol | PIN Asignado |
| :--- | :--- | :--- |
| `USR-001-ADMIN` | Administrador | `1234` |
| `USR-002-JUAN` | Operador | `2580` |
| `USR-003-ANA` | Operador | `9014` |

---

## 🔄 Flujo Operativo y Registro de Eventos

1. **Estado en Espera:** El sistema aguarda actividad en el sensor PIR mientras mantiene la conexión WiFi (`Wokwi-GUEST`).
2. **Detección de Movimiento:** Al activar el PIR, se imprime `Movimiento detectado` y se solicita `Ingrese PIN`.
3. **Validación:**
   * **PIN Correcto:** Se registra `[EVENTO] ACCESS_GRANTED <ID_USUARIO> intento=<N>`, se enciende el LED verde y se abre el servo por 4 segundos.
   * **Autocierre:** Transcurrido el pulso, el servo regresa a $0^\circ$, se registra `[EVENTO] DOOR_LOCKED <ID_USUARIO>` y el sistema vuelve a `[Sistema en espera]`.
   * **PIN Incorrecto (3 veces):** Se activa el bloqueo por 30 segundos con parpadeo del LED rojo cada 300 ms.

---

## 🚀 Cómo Ejecutar la Simulación en Wokwi

1. Abre el proyecto en **Wokwi** con los archivos `esp32-http-server.ino`, `diagram.json` y `libraries.txt`.
2. Inicia la simulación haciendo clic en el botón **Start Simulation** (▶).
3. Haz clic sobre el **Sensor PIR** en el diagrama y selecciona **Simulate motion**.
4. En el monitor serial aparecerá la solicitud `Ingrese PIN`. Utiliza el **Teclado 4x4** virtual para digitar uno de los PIN autorizados (por ejemplo, `1234` o `2580`).
5. Observa el giro del servomotor, el encendido del LED verde y la traza de eventos en tiempo real en la consola.

---

## 📈 Mejoras Recomendadas para Producción

* **Seguridad Criptográfica:** Reemplazar los PIN almacenados en texto plano por hashes **SHA-256** con *salt*.
* **Almacenamiento Persistente (NVS):** Guardar los logs de auditoría y credenciales en la memoria no volátil del ESP32 para conservar el historial ante cortes de energía.
* **Cifrado de Transporte:** Migrar el servidor HTTP local a **HTTPS** o **MQTTS** para proteger la telemetría en red.