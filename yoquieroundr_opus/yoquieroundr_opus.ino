/**
 * ESP32-WROVER-E | INMP441 + MAX98357A
 * LittleFS + WiFi File Upload Server + OPUS Playback
 *
 * Librerías requeridas (Library Manager):
 *  - ESP Async WebServer  (by lacamera)
 *  - AsyncTCP             (by dvarrel)
 *  - ESP32-audioI2S       (by schreibfaul1)  <-- maneja OPUS + I2S
 *
 * Pines por defecto (ajusta si es necesario):
 *  MAX98357A (I2S out):
 *    BCLK  → GPIO 27
 *    LRC   → GPIO 26
 *    DIN   → GPIO 25
 *
 *  INMP441 (I2S in) — no usado en este sketch, reservado:
 *    SCK   → GPIO 32
 *    WS    → GPIO 33
 *    SD    → GPIO 34
 */

#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <Audio.h>   // ESP32-audioI2S

// ─── Configuración WiFi ──────────────────────────────────────────────────────
const char* WIFI_SSID = "TU_SSID";
const char* WIFI_PASS = "TU_PASSWORD";

// ─── Pines I2S → MAX98357A ───────────────────────────────────────────────────
#define I2S_BCLK  27
#define I2S_LRC   26
#define I2S_DOUT  25

// ─── Objetos globales ────────────────────────────────────────────────────────
AsyncWebServer server(80);
Audio audio;

// Archivo que se reproducirá (se actualiza al subir)
String currentFile = "";
bool shouldPlay    = false;

// ─── HTML de la interfaz web ──────────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 OPUS Player</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: 'Courier New', monospace;
      background: #0d0d0d;
      color: #e0e0e0;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .card {
      background: #1a1a1a;
      border: 1px solid #2a2a2a;
      border-radius: 12px;
      padding: 2rem;
      width: 100%;
      max-width: 420px;
      box-shadow: 0 0 40px rgba(0,255,180,0.05);
    }
    h1 {
      font-size: 1rem;
      color: #00ffb4;
      letter-spacing: 0.2em;
      text-transform: uppercase;
      margin-bottom: 0.25rem;
    }
    .subtitle {
      font-size: 0.7rem;
      color: #555;
      margin-bottom: 2rem;
      letter-spacing: 0.1em;
    }
    .drop-zone {
      border: 2px dashed #2a2a2a;
      border-radius: 8px;
      padding: 2.5rem 1rem;
      text-align: center;
      cursor: pointer;
      transition: all 0.2s;
      margin-bottom: 1rem;
      position: relative;
    }
    .drop-zone:hover, .drop-zone.dragover {
      border-color: #00ffb4;
      background: rgba(0,255,180,0.03);
    }
    .drop-zone input[type=file] {
      position: absolute;
      inset: 0;
      opacity: 0;
      cursor: pointer;
      width: 100%;
      height: 100%;
    }
    .drop-icon { font-size: 2rem; margin-bottom: 0.5rem; }
    .drop-text { font-size: 0.8rem; color: #666; }
    .drop-text span { color: #00ffb4; }
    #fileName {
      font-size: 0.75rem;
      color: #888;
      margin-bottom: 1rem;
      min-height: 1rem;
      text-align: center;
    }
    .btn {
      width: 100%;
      padding: 0.75rem;
      background: transparent;
      color: #00ffb4;
      border: 1px solid #00ffb4;
      border-radius: 6px;
      font-family: inherit;
      font-size: 0.8rem;
      letter-spacing: 0.15em;
      text-transform: uppercase;
      cursor: pointer;
      transition: all 0.2s;
      margin-bottom: 0.75rem;
    }
    .btn:hover { background: #00ffb4; color: #0d0d0d; }
    .btn:disabled { opacity: 0.3; cursor: not-allowed; }
    .btn-play {
      border-color: #00b4ff;
      color: #00b4ff;
    }
    .btn-play:hover { background: #00b4ff; color: #0d0d0d; }
    .btn-stop {
      border-color: #ff4466;
      color: #ff4466;
      margin-bottom: 0;
    }
    .btn-stop:hover { background: #ff4466; color: #0d0d0d; }
    #progress {
      height: 3px;
      background: #2a2a2a;
      border-radius: 2px;
      margin: 1rem 0;
      overflow: hidden;
    }
    #progressBar {
      height: 100%;
      width: 0%;
      background: #00ffb4;
      transition: width 0.1s;
    }
    #status {
      font-size: 0.7rem;
      color: #555;
      text-align: center;
      letter-spacing: 0.1em;
      min-height: 1.2rem;
    }
    #status.ok  { color: #00ffb4; }
    #status.err { color: #ff4466; }
    .files-section { margin-top: 1.5rem; }
    .files-label {
      font-size: 0.65rem;
      color: #444;
      letter-spacing: 0.15em;
      text-transform: uppercase;
      margin-bottom: 0.5rem;
    }
    #fileList { list-style: none; }
    #fileList li {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 0.5rem 0.75rem;
      border-radius: 4px;
      font-size: 0.75rem;
      color: #888;
      cursor: pointer;
      transition: background 0.15s;
    }
    #fileList li:hover { background: #222; color: #e0e0e0; }
    #fileList li .play-btn { color: #00b4ff; font-size: 0.65rem; }
  </style>
</head>
<body>
<div class="card">
  <h1>ESP32 OPUS</h1>
  <div class="subtitle">WROVER-E &bull; MAX98357A &bull; LittleFS</div>

  <div class="drop-zone" id="dropZone">
    <input type="file" id="fileInput" accept=".opus,.mp3,.wav">
    <div class="drop-icon">📁</div>
    <div class="drop-text">Arrastra tu archivo o <span>haz clic aquí</span></div>
  </div>
  <div id="fileName">Ningún archivo seleccionado</div>

  <div id="progress"><div id="progressBar"></div></div>

  <button class="btn" id="btnUpload" onclick="uploadFile()" disabled>▲ Subir archivo</button>
  <button class="btn btn-play" id="btnPlay" onclick="playAudio()" disabled>▶ Reproducir</button>
  <button class="btn btn-stop" onclick="stopAudio()">■ Detener</button>

  <div id="status">Listo</div>

  <div class="files-section">
    <div class="files-label">Archivos en flash</div>
    <ul id="fileList"></ul>
  </div>
</div>

<script>
  let selectedFile = null;

  const dropZone = document.getElementById('dropZone');
  const fileInput = document.getElementById('fileInput');

  dropZone.addEventListener('dragover', e => { e.preventDefault(); dropZone.classList.add('dragover'); });
  dropZone.addEventListener('dragleave', () => dropZone.classList.remove('dragover'));
  dropZone.addEventListener('drop', e => {
    e.preventDefault();
    dropZone.classList.remove('dragover');
    if (e.dataTransfer.files[0]) handleFile(e.dataTransfer.files[0]);
  });
  fileInput.addEventListener('change', () => { if (fileInput.files[0]) handleFile(fileInput.files[0]); });

  function handleFile(file) {
    selectedFile = file;
    document.getElementById('fileName').textContent = `${file.name}  (${(file.size/1024).toFixed(1)} KB)`;
    document.getElementById('btnUpload').disabled = false;
  }

  function setStatus(msg, type='') {
    const s = document.getElementById('status');
    s.textContent = msg;
    s.className = type;
  }

  async function uploadFile() {
    if (!selectedFile) return;
    const formData = new FormData();
    formData.append('file', selectedFile, selectedFile.name);

    setStatus('Subiendo...');
    document.getElementById('btnUpload').disabled = true;

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/upload');
    xhr.upload.onprogress = e => {
      if (e.lengthComputable) {
        document.getElementById('progressBar').style.width = (e.loaded/e.total*100)+'%';
      }
    };
    xhr.onload = () => {
      document.getElementById('progressBar').style.width = '100%';
      if (xhr.status === 200) {
        setStatus('✓ Subido correctamente', 'ok');
        document.getElementById('btnPlay').disabled = false;
        document.getElementById('btnPlay').dataset.file = '/' + selectedFile.name;
        loadFileList();
      } else {
        setStatus('✗ Error al subir', 'err');
      }
      document.getElementById('btnUpload').disabled = false;
      setTimeout(() => { document.getElementById('progressBar').style.width = '0%'; }, 1500);
    };
    xhr.onerror = () => { setStatus('✗ Error de red', 'err'); };
    xhr.send(formData);
  }

  async function playAudio(filename) {
    const f = filename || document.getElementById('btnPlay').dataset.file;
    if (!f) return;
    setStatus('▶ Reproduciendo: ' + f, 'ok');
    const res = await fetch('/play?file=' + encodeURIComponent(f));
    if (!res.ok) setStatus('✗ Error al reproducir', 'err');
  }

  async function stopAudio() {
    await fetch('/stop');
    setStatus('■ Detenido');
  }

  async function loadFileList() {
    const res = await fetch('/files');
    const files = await res.json();
    const ul = document.getElementById('fileList');
    ul.innerHTML = '';
    files.forEach(f => {
      const li = document.createElement('li');
      li.innerHTML = `<span>${f.name} <small>(${(f.size/1024).toFixed(1)} KB)</small></span><span class="play-btn">▶ play</span>`;
      li.onclick = () => playAudio('/' + f.name);
      ul.appendChild(li);
    });
  }

  loadFileList();
</script>
</body>
</html>
)rawliteral";

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  // 1. LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Error montando LittleFS");
    return;
  }
  Serial.println("[FS] LittleFS montado OK");

  // 2. WiFi
  Serial.print("[WiFi] Conectando a ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[WiFi] Conectado — IP: ");
  Serial.println(WiFi.localIP());

  // 3. I2S / Audio
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(12);  // 0–21

  // ── Rutas del servidor ───────────────────────────────────────────────────

  // Página principal
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", INDEX_HTML);
  });

  // Lista de archivos en JSON
  server.on("/files", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "[";
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    bool first = true;
    while (f) {
      if (!f.isDirectory()) {
        if (!first) json += ",";
        json += "{\"name\":\"" + String(f.name()) + "\",\"size\":" + f.size() + "}";
        first = false;
      }
      f = root.openNextFile();
    }
    json += "]";
    req->send(200, "application/json", json);
  });

  // Subir archivo
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      req->send(200, "text/plain", "OK");
    },
    [](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
      static File uploadFile;

      if (index == 0) {
        String path = "/" + filename;
        Serial.printf("[Upload] Iniciando: %s\n", path.c_str());
        if (LittleFS.exists(path)) LittleFS.remove(path);
        uploadFile = LittleFS.open(path, FILE_WRITE);
        if (!uploadFile) Serial.println("[Upload] Error abriendo archivo");
      }

      if (uploadFile && len > 0) {
        uploadFile.write(data, len);
      }

      if (final) {
        if (uploadFile) {
          uploadFile.close();
          Serial.printf("[Upload] Completo: %u bytes\n", index + len);
          currentFile = "/" + filename;
        }
      }
    }
  );

  // Reproducir
  server.on("/play", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("file")) {
      String path = req->getParam("file")->value();
      if (LittleFS.exists(path)) {
        audio.stopSong();
        currentFile = path;
        shouldPlay  = true;
        req->send(200, "text/plain", "Playing: " + path);
        Serial.printf("[Audio] Reproduciendo: %s\n", path.c_str());
      } else {
        req->send(404, "text/plain", "Archivo no encontrado: " + path);
      }
    } else {
      req->send(400, "text/plain", "Falta parametro ?file=");
    }
  });

  // Detener
  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest* req) {
    audio.stopSong();
    shouldPlay = false;
    req->send(200, "text/plain", "Stopped");
    Serial.println("[Audio] Detenido");
  });

  server.begin();
  Serial.println("[Server] HTTP server activo");
  Serial.printf("[Server] Abre en tu browser: http://%s\n", WiFi.localIP().toString().c_str());
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  // Iniciar reproducción desde el loop (audio.loop() es bloqueante-friendly)
  if (shouldPlay && currentFile.length() > 0) {
    shouldPlay = false;
    audio.connecttoFS(LittleFS, currentFile.c_str());
  }

  audio.loop();  // DEBE estar en loop(), maneja el stream I2S internamente
}

// ─── Callbacks opcionales de ESP32-audioI2S ──────────────────────────────────
void audio_info(const char* info)        { Serial.printf("[Audio] %s\n", info); }
void audio_eof_mp3(const char* info)     { Serial.printf("[Audio] EOF: %s\n", info); }
