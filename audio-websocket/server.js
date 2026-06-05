//
//  server.js  —  WebSocket bridge + rolling audio archive
//
//  Audio batches from ESP32 are accumulated in a rolling ring buffer
//  (last 4000 batches = ~8 MB).  The monitor page can merge any slice
//  into a downloadable WAV file via the /wav endpoint.
//

const http       = require('http');
const fs         = require('fs');
const path       = require('path');
const os         = require('os');
const { WebSocketServer } = require('ws');
const mdns       = require('multicast-dns')();

const PORT       = 8080;

// ================================================================
//  mDNS helper — pick the best local IPv4 address to advertise
//  (skip virtual/loopback, prefer Ethernet over Wi-Fi)
// ================================================================
function getLocalIP() {
    const interfaces = os.networkInterfaces();
    const candidates = [];
    for (const name of Object.keys(interfaces)) {
        for (const iface of interfaces[name]) {
            if (iface.family === 'IPv4' && !iface.internal) {
                const addr = iface.address;
                // Skip well-known virtual/VM IP ranges
                if (addr.startsWith('127.')) continue;           // loopback
                if (addr.startsWith('192.168.56.')) continue;      // VirtualBox host-only
                if (addr.startsWith('192.168.99.')) continue;      // Vagrant / VirtualBox
                if (addr.startsWith('172.29.')) continue;          // WSL / Hyper-V
                if (addr.startsWith('172.17.')) continue;          // Docker default
                if (addr.startsWith('10.0.75.')) continue;         // Docker for Windows
                if (addr.startsWith('169.254.')) continue;         // Link-local / APIPA
                candidates.push({ name, address: addr });
            }
        }
    }
    // Prefer Wi-Fi first (since user is on Wi-Fi now), then Ethernet, then anything else
    const wifi = candidates.find(c => c.name.includes('Wi-Fi') || c.name.includes('Wireless'));
    if (wifi) return wifi.address;
    const eth = candidates.find(c => c.name.includes('Ethernet'));
    if (eth) return eth.address;
    return candidates[0] ? candidates[0].address : '0.0.0.0';
}
const LOCAL_IP = getLocalIP();
const AUTH_TOKEN = process.env.AUTH_TOKEN || '';
if (!AUTH_TOKEN) {
    console.error('FATAL: AUTH_TOKEN environment variable is not set');
    process.exit(1);
}
const MIME_TYPES = {
    '.html': 'text/html',
    '.css':  'text/css',
    '.js':   'application/javascript',
    '.svg':  'image/svg+xml',
};

// ================================================================
//  Rolling ring buffer — stores raw PCM chunks from ESP32
// ================================================================
class RollingBuffer {
    constructor(capacity) {
        this.capacity = capacity;
        this.buffers = [];
    }

    push(buf) {
        // Store a copy so we don't hold references to ws Buffer pools
        this.buffers.push(Buffer.from(buf));
        while (this.buffers.length > this.capacity) {
            this.buffers.shift();
        }
    }

    getLast(count) {
        if (count <= 0) return [];
        const start = Math.max(0, this.buffers.length - count);
        return this.buffers.slice(start);
    }

    get length() {
        return this.buffers.length;
    }
}

// ================================================================
//  WAV file builder  (RIFF/WAVE, PCM 16-bit little-endian)
// ================================================================
function buildWav(pcmBuffers, sampleRate = 16000, numChannels = 1, bitsPerSample = 16) {
    const pcmLength = pcmBuffers.reduce((sum, b) => sum + b.length, 0);
    const headerSize = 44;
    const totalSize = headerSize + pcmLength;

    const byteRate = sampleRate * numChannels * bitsPerSample / 8;
    const blockAlign = numChannels * bitsPerSample / 8;

    const wav = Buffer.alloc(totalSize);

    // RIFF header
    wav.write('RIFF', 0);
    wav.writeUInt32LE(36 + pcmLength, 4);
    wav.write('WAVE', 8);

    // fmt chunk
    wav.write('fmt ', 12);
    wav.writeUInt32LE(16, 16);
    wav.writeUInt16LE(1, 20);           // PCM
    wav.writeUInt16LE(numChannels, 22);
    wav.writeUInt32LE(sampleRate, 24);
    wav.writeUInt32LE(byteRate, 28);
    wav.writeUInt16LE(blockAlign, 32);
    wav.writeUInt16LE(bitsPerSample, 34);

    // data chunk
    wav.write('data', 36);
    wav.writeUInt32LE(pcmLength, 40);

    // Write PCM data
    let offset = 44;
    for (const buf of pcmBuffers) {
        buf.copy(wav, offset);
        offset += buf.length;
    }

    return wav;
}

// ================================================================
//  Shared state
// ================================================================
let stats = {
    batchCount:   0,
    totalBytes:   0,
    startTime:    Date.now(),
    lastBatchAt:  0,
};

const rollingBuffer = new RollingBuffer(4000);

// ================================================================
//  HTTP server
// ================================================================
const server = http.createServer((req, res) => {
    const urlPath = req.url;

    // ---------- WAV download / playback endpoint ----------
    if (urlPath.startsWith('/wav')) {
        const parts = urlPath.split('?');
        const params = new URLSearchParams(parts[1] || '');
        let count = parseInt(params.get('count')) || 1000;

        const available = rollingBuffer.length;
        if (count > available) count = available;
        if (count <= 0) {
            res.writeHead(400, { 'Content-Type': 'text/plain' });
            res.end('No audio data available yet.\n');
            return;
        }

        const buffers = rollingBuffer.getLast(count);
        const wav = buildWav(buffers);

        const isDownload = params.get('download') === '1';
        const filename = `recording_${count}batches.wav`;

        res.writeHead(200, {
            'Content-Type': 'audio/wav',
            'Content-Length': wav.length,
            'Content-Disposition': isDownload
                ? `attachment; filename="${filename}"`
                : `inline; filename="${filename}"`,
        });
        res.end(wav);
        return;
    }

    // ---------- Static files ----------
    let filePath = urlPath === '/' ? '/index.html' : urlPath;
    filePath = path.join(__dirname, 'public', filePath);

    const ext = path.extname(filePath);
    fs.readFile(filePath, (err, data) => {
        if (err) {
            res.writeHead(404);
            res.end('Not found');
            return;
        }
        res.writeHead(200, { 'Content-Type': MIME_TYPES[ext] || 'text/plain' });
        res.end(data);
    });
});

// ================================================================
//  WebSocket server
// ================================================================
const wss = new WebSocketServer({ server });

wss.on('connection', (ws, req) => {
    // Tag the connection with its path — ws v8+ does not auto‑set ws.url
    const url = req.url || '/';
    ws._path = url;

    if (ws._path === '/audio') {
        // Reject ESP32 connections with a bad or missing auth token
        const auth = (req.headers.authorization || '').replace(/^Bearer\s+/i, '');
        if (auth !== AUTH_TOKEN) {
            console.log(`[ws] Rejecting ${req.socket.remoteAddress} — invalid token`);
            ws.close(4401, 'Unauthorized');
            return;
        }
        // ---------- ESP32 audio upload path ----------
        console.log(`[audio] ESP32 connected from ${req.socket.remoteAddress}`);

        ws.on('message', (data) => {
            const len = Buffer.isBuffer(data) ? data.length : 0;

            // Update stats
            stats.batchCount++;
            stats.totalBytes += len;
            stats.lastBatchAt = Date.now();

            // ---- Archive raw PCM into rolling buffer ----
            if (len > 0) {
                rollingBuffer.push(data);
            }

            // Extract first 512 samples (1024 bytes) for waveform preview
            let preview = null;
            if (len >= 1024) {
                const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
                preview = [];
                for (let i = 0; i < 1024; i += 2) {
                    preview.push(buf.readInt16LE(i));
                }
            }

            // Broadcast monitor update to all /monitor clients
            const payload = JSON.stringify({
                type:       'batch',
                batchId:    stats.batchCount,
                batchSize:  len,
                totalBytes: stats.totalBytes,
                elapsedMs:  Date.now() - stats.startTime,
                preview:    preview,         // first 512 int16 samples
            });

            // Log first few batches for debugging
            if (stats.batchCount <= 3) {
                console.log(`[audio] batch #${stats.batchCount}: ${len}B, `
                    + `preview=${preview ? preview.length + ' samples' : 'null'}, `
                    + `samples[0..2]=${preview ? preview.slice(0,3).join(',') : 'n/a'}`);
            }

            wss.clients.forEach((client) => {
                if (client._path === '/monitor' && client.readyState === ws.OPEN) {
                    client.send(payload);
                }
            });
        });

        ws.on('close', () => {
            console.log('[audio] ESP32 disconnected');
        });

        ws.on('error', (err) => {
            console.error('[audio] Error:', err.message);
        });

    } else if (ws._path === '/monitor') {
        // ---------- Browser monitor path ----------
        console.log(`[monitor] Browser connected from ${req.socket.remoteAddress}`);

        // Send current stats on connect
        ws.send(JSON.stringify({
            type:       'stats',
            batchCount: stats.batchCount,
            totalBytes: stats.totalBytes,
            elapsedMs:  Date.now() - stats.startTime,
        }));

        ws.on('close', () => {
            console.log('[monitor] Browser disconnected');
        });

    } else {
        console.log(`[ws] Unknown path "${url}" — closing`);
        ws.close(4000, 'Unknown path');
    }
});

server.listen(PORT, '0.0.0.0', () => {
    console.log(`\n  Audio WebSocket server running on:`);
    console.log(`    Monitor  → http://0.0.0.0:${PORT}/`);
    console.log(`    ESP32    → ws://<your-ip>:${PORT}/audio`);
    console.log(`    Monitor  → ws://<your-ip>:${PORT}/monitor`);
    console.log(`    WAV      → http://<your-ip>:${PORT}/wav?count=1000\n`);

    // Start mDNS advertisement
    console.log(`  mDNS advertising as: audio-webserver.local (${LOCAL_IP})\n`);
});

// ================================================================
//  mDNS responder — answer queries for audio-webserver.local
// ================================================================
mdns.on('query', (query) => {
    const shouldRespond = query.questions.some(
        q => q.name === 'audio-webserver.local' && q.type === 'A'
    );
    if (shouldRespond) {
        mdns.respond({
            answers: [{
                name: 'audio-webserver.local',
                type: 'A',
                ttl: 300,
                data: LOCAL_IP,
            }],
        });
    }
});
