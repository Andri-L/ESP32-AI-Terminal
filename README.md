# BMO — Interfaz Física de Voz con IA

**"Un robot de voz que escucha, piensa y responde — construido desde cero."**

BMO es un dispositivo físico de voz que funciona como interfaz de usuario para un agente de inteligencia artificial. El dispositivo captura audio del entorno, lo envía a un servidor, procesa la respuesta con IA y la reproduce en voz.

---

## Hardware

| Componente | Especificaciones | Estado |
|---|---|---|
| **ESP32-WROVER-E** | DevKitC, USB-C, PSRAM 8MB, WiFi + BT, Dual-core 240 MHz | ✅ Soldado |
| **Micrófono I2S** | INMP441, digital omnidireccional, SNR 61 dB | ✅ Soldado |
| **Amplificador I2S** | MAX98357A, Clase D, 3.2W, bornera verde | ✅ Soldado |
| **Altavoz** | 4Ω 3W, mini de cavidad | ✅ Listo |
| **Protoboard** | MB-102, 830 puntos, 3.3V/5V | ✅ Listo |

---

## Arquitectura del Sistema

El sistema se divide en dos capas independientes que se comunican por red:

```
┌─────────────────────────────────────────────────────────────┐
│                     HARDWARE (ESP32)                        │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐              │
│  │ INMP441  │───▶│  I2S/DMA │───▶│ WebSocket│───▶ Internet │
│  │ (Mic)    │    │  Buffer  │    │  Client  │              │
│  └──────────┘    └──────────┘    └──────────┘              │
│                                                                  │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐              │
│  │ MAX98357A│◀───│  I2S Out │◀───│  Audio   │◀─── Internet │
│  │ (Amp)    │    │  Driver  │    │  Stream  │              │
│  └──────────┘    └──────────┘    └──────────┘              │
│                                                                  │
│  WiFi: 2.4 GHz                                                    │
│  Protocolo: I2S (audio digital)                                  │
└─────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                  SERVIDOR (Node.js — Debug)                   │
│  • WebSocket: recepción de audio en tiempo real               │
│  • Rolling buffer: últimos 4000 batches (~8 MB)               │
│  • WAV builder: descarga de grabaciones                       │
│  • Monitor web: dashboard en tiempo real                      │
│  • mDNS: autodescubrimiento (`audio-webserver.local`)         │
│  • Auth: token Bearer para seguridad del stream ESP32         │
│                                                              │
│  ⚠️ Solo para desarrollo y monitoreo. En producción,         │
│     el ESP32 envía audio directamente a GoAgent.              │
└─────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                     GoAgent (Golang)                          │
│  • WebSocket /audio: ingestión de audio PCM del ESP32        │
│  • VAD: Voice Activity Detection (RMS → dBFS)                │
│  • ASR: Hugging Face Whisper (speech-to-text)                │
│  • LLM: Groq / OpenAI-compatible (ReAct loop + tools)        │
│  • Session memory: conversación multi-turno                  │
│                                                              │
│  ✅ Operacional — desplegado en OCI Free VPS                 │
└─────────────────────────────────────────────────────────────┘
```

### Flujo de Operación Completo

| Paso | Componente | Acción |
|---|---|---|
| 1 | INMP441 | Captura audio analógico del usuario |
| 2 | ESP32-WROVER-E | Recibe señal I2S y la procesa en buffer PSRAM |
| 3 | ESP32 → WiFi | Envía audio PCM al servidor vía WebSocket |
| 4 | GoAgent `/audio` | Recibe batches PCM binarios (2048 bytes cada ~64ms) |
| 5 | GoAgent VAD | Detecta inicio/fin de habla (RMS → dBFS) |
| 6 | GoAgent ASR | Construye WAV en memoria → Hugging Face Whisper → texto |
| 7 | GoAgent LLM | Procesa texto con ReAct loop vía Groq API |
| 8 | GoAgent | Loggea la respuesta textual (TTS planificado para futuro) |

> ⚠️ **Nota:** El dispositivo requiere conexión WiFi activa. No procesa IA localmente; el ESP32 actúa como interfaz de audio y red.

---

## Diagrama de Conexiones (Pinout)

### INMP441 → ESP32

| Pin INMP441 | Pin ESP32 (GPIO) | Cable | Descripción |
|---|---|---|---|
| VDD | 3.3V | Rojo | Alimentación 3.3V |
| GND | GND | Negro | Tierra común |
| SD (DATA) | GPIO 34 | Verde | Datos de audio I2S |
| WS (LRCK) | GPIO 25 | Amarillo | Selección de canal L/R |
| SCK (CLK) | GPIO 26 | Azul | Reloj de bit I2S |
| L/R | GND | Negro | Fijar canal izquierdo (L) |

### MAX98357A → ESP32

| Pin MAX98357A | Pin ESP32 (GPIO) | Cable | Descripción |
|---|---|---|---|
| VIN | 5V (VUSB) | Rojo | Alimentación 5V |
| GND | GND | Negro | Tierra común |
| DIN (DATA) | GPIO 22 | Verde | Datos de audio I2S |
| LRC (LRCK) | GPIO 25 | Amarillo | **Compartido con INMP441** |
| BCLK (CLK) | GPIO 26 | Azul | **Compartido con INMP441** |
| SD (Shutdown) | 3.3V o NC | — | Dejar en alto para activar |
| GAIN | NC | — | Sin conectar = ganancia 9 dB |

> 🔴 **Importante:** LRC y BCLK se comparten entre INMP441 y MAX98357A. El bus I2S es compartido; el ESP32 actúa como maestro de ambos.

### Altavoz → MAX98357A

| Bornera | Cable | Descripción |
|---|---|---|
| + (positivo) | Rojo (o marcado) | Señal de audio positiva |
| — (negativo) | Negro (o sin marcar) | Señal de audio negativa |

---

## Estructura del Proyecto

```
ESP32-AI-Terminal/
├── audio_batch_proc/           ← Firmware ESP32 (Arduino C++)
│   ├── audio_batch_proc.ino    ← Main sketch: I2S, WiFi, mDNS, WebSocket
│   ├── audio_mic.cpp/h         ← Driver I2S del INMP441
│   ├── websocket.cpp/h         ← Cliente WebSocket custom (RFC 6455)
│   └── ...
├── audio-websocket/            ← Servidor Node.js
│   ├── server.js               ← WebSocket + HTTP + mDNS + Auth
│   ├── public/index.html         ← Dashboard monitor
│   ├── package.json
│   └── ...
├── for_elisa_bethoven.ino/     ← Sketch de prueba (reproducción audio)
├── wifi_test/                  ← Sketch de prueba (WiFi)
├── yoquieroundr_opus/          ← Sketch de prueba (Opus)
└── .gitignore
```

---

## Cómo ejecutar

### 1. Servidor Node.js (solo desarrollo/monitoreo)

El servidor Node.js es para debugging y monitoreo local. **En producción**, el ESP32 envía audio directamente al WebSocket de GoAgent en el VPS.

```powershell
# Desde el directorio audio-websocket/
pnpm install

# Configurar el token de autenticación
$env:AUTH_TOKEN="tu-token-secreto"

# Iniciar
pnpm start
```

Salida esperada:
```
Audio WebSocket server running on:
  Monitor  → http://0.0.0.0:8080/
  ESP32    → ws://<tu-ip>:8080/audio
  Monitor  → ws://<tu-ip>:8080/monitor
  WAV      → http://<tu-ip>:8080/wav?count=1000

mDNS advertising as: audio-webserver.local (192.168.80.15)
```

### 2. ESP32

1. Abrir `audio_batch_proc/audio_batch_proc.ino` en Arduino IDE
2. Configurar credenciales WiFi en `audio_batch_proc.ino`:
   ```cpp
   #define WIFI_SSID     "TU_RED_WIFI"
   #define WIFI_PASSWORD "TU_CONTRASEÑA"
   ```
3. Asegurar que `AUTH_TOKEN` coincida con el del servidor
4. Seleccionar placa: **ESP32 WROVER Module**
5. Compilar y flashear

El ESP32 intentará:
1. Conectarse a WiFi
2. Resolver `audio-webserver.local` vía mDNS (entorno local)
3. Si mDNS falla, usar `WS_FALLBACK_URL` → `ws://149.130.179.83:8080/audio` (GoAgent en VPS)
4. Conectarse al WebSocket con token de autenticación
5. Iniciar streaming de audio

---

## Endpoints del Servidor

### WebSocket

| Path | Auth | Descripción |
|---|---|---|
| `/audio` | ✅ Bearer token | Stream de audio PCM desde el ESP32 |
| `/monitor` | ❌ Libre | Dashboard web en tiempo real |

### HTTP

| Ruta | Método | Descripción |
|---|---|---|
| `/` | GET | Dashboard monitor (HTML) |
| `/wav?count=1000&download=1` | GET | Descargar últimas N muestras como WAV |

---

## Decisiones Pendientes

| Prioridad | Decisión | Opciones | Impacto |
|---|---|---|---|
| ✅ Decidido | Servicio STT | **Hugging Face Whisper** (`whisper-large-v3-turbo`) | Integrado en GoAgent |
| ✅ Decidido | LLM | **Groq API** (OpenAI-compatible, `llama-3.3-70b-versatile`) | Integrado en GoAgent |
| 🔴 Alta | Servicio TTS | Piper (local) / OpenAI TTS / ElevenLabs | Calidad de voz |
| 🟡 Media | Formato de audio | WAV / PCM raw / Opus | Uso de red y memoria |
| 🟡 Media | Detección de voz | Botón físico / VAD / Wake word | VAD implementado en GoAgent |
| 🟢 Baja | Frecuencia de muestreo | 16000 Hz / 44100 Hz | Tamaño de buffer |

---

## Estado Actual

- ✅ Fase 0 — Materiales adquiridos y soldados
- ✅ Captura I2S desde INMP441 (16000 Hz, 16-bit, mono)
- ✅ Streaming WebSocket en tiempo real
- ✅ Servidor Node.js con buffer circular y monitor web
- ✅ Descarga de audio como WAV
- ✅ mDNS autodescubrimiento (`audio-webserver.local`)
- ✅ Autenticación Bearer token en WebSocket
- ✅ **Integración con GoAgent** — WebSocket directo ESP32 → GoAgent
- ✅ **VAD + ASR** — Detección de voz y transcripción con Whisper
- ✅ **LLM con herramientas** — ReAct loop vía Groq API
- 🔄 **Pendiente:** TTS + reproducción de audio (I2S out → MAX98357A)

---

## Troubleshooting

| Síntoma | Causa probable | Solución |
|---|---|---|
| ESP32 no conecta WiFi | Red 5 GHz o credenciales mal | ESP32 solo soporta 2.4 GHz |
| TCP connect failed | Laptop en otra red | Verificar que ambos estén en mismo subnet (192.168.x.x) |
| mDNS falla | Firewall Windows | Desactivar firewall temporalmente para UDP 5353 |
| Handshake timeout | Token incorrecto | Verificar que `AUTH_TOKEN` coincida en server y .ino |
| Audio no se reproduce | Formato WAV incompatible | Servidor debe devolver PCM 16-bit 16000 Hz mono |
| Altavoz con ruido (hiss) | GND flotante | Asegurar GND común entre ESP32 y MAX98357A |

---

## Créditos

- Proyecto **BMO** — Documentación Técnica v1.0
- Estado: **Fase 0** — Materiales adquiridos
- Licencia: MIT

```
[ BMO ]
Interfaz Física de Voz con IA
"Un robot de voz que escucha, piensa y responde."
```
