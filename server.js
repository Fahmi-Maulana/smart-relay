const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const multer = require('multer');
const fs = require('fs');
const path = require('path');

const app = express();
const server = http.createServer(app);

// ==========================================================
// 1. INISIALISASI DIREKTORI
// ==========================================================
// Mencegah server crash saat Multer mencoba menyimpan file OTA
// ke folder yang belum ada.
const publicDir = path.join(__dirname, 'public');
if (!fs.existsSync(publicDir)) {
    fs.mkdirSync(publicDir);
    console.log(`[SYSTEM] Folder /public berhasil dibuat.`);
}

// ==========================================================
// 2. KONFIGURASI SOCKET.IO (Dukungan EIO3 untuk ESP32)
// ==========================================================
const io = new Server(server, {
    cors: { origin: "*" },
    allowEIO3: true, 
    transports: ['websocket', 'polling']
});

// ==========================================================
// 3. KONFIGURASI PIPELINE OTA (MULTER)
// ==========================================================
const storage = multer.diskStorage({
    destination: (req, file, cb) => { 
        cb(null, publicDir); 
    },
    filename: (req, file, cb) => { 
        cb(null, 'firmware.bin'); // Paksa penamaan statis agar ditimpa
    }
});
const upload = multer({ storage: storage });

// ==========================================================
// 4. MIDDLEWARE & STATE GLOBAL
// ==========================================================
app.use(express.static('public')); // Menjadikan folder public sebagai Web Server

let lastSensorData = null;
let currentLimits = { i: 2.0, p: 440 };

// ==========================================================
// 5. REST API ENDPOINT (FILE UPLOADER)
// ==========================================================
app.post('/upload-ota', upload.single('firmware'), (req, res) => {
    if (!req.file) {
        console.error(`[OTA] ERROR: File gagal diunggah!`);
        return res.status(400).send('GAGAL: File tidak ditemukan.');
    }
    
    console.log(`[OTA] Bundle biner diterima. Memicu ESP32...`);
    io.emit('serverToEsp', { cmd: 'startOta' }); // Tembak sinyal eksekusi ke hardware
    res.send('Deploy sukses! ESP32 sedang menarik firmware...');
});

// ==========================================================
// 6. SOCKET.IO BROKER (PENGATUR LALU LINTAS DATA)
// ==========================================================
io.on('connection', (socket) => {
    console.log(`[+] Client Connected : ${socket.id}`);
    
    // A. Sinkronisasi Awal (Mencegah UI kosong saat web baru direfresh)
    socket.emit('updateLimits', currentLimits);
    if (lastSensorData) {
        socket.emit('sensorData', lastSensorData);
    }

    // B. Menerima telemetri dari ESP32 -> Lempar ke Web
    socket.on('espData', (data) => {
        lastSensorData = data;
        socket.broadcast.emit('sensorData', data); // Broadcast ke semua tab web terbuka
    });

    // C. Menerima limit dari Web -> Lempar ke ESP32 & Web lain
    socket.on('setLimits', (limits) => {
        currentLimits = limits;
        console.log(`[LIMIT] Update diterima -> Arus: ${limits.i}A | Daya: ${limits.p}W`);
        socket.broadcast.emit('updateLimits', limits); // Sinkronisasi antar-tab browser
        io.emit('serverToEsp', { cmd: 'applyLimits', data: limits });
    });

    // D. Menerima konfigurasi statis jaringan dari Web -> Lempar ke ESP32
    socket.on('setNetwork', (netConfig) => {
        console.log(`[NETWORK] Push Config -> IP: ${netConfig.ip} | GW: ${netConfig.gw}`);
        io.emit('serverToEsp', { cmd: 'applyNetwork', data: netConfig });
    });

    // E. Menerima perintah absolut (Reset, dll) dari Web -> Lempar ke ESP32
    socket.on('command', (cmd) => {
        console.log(`[COMMAND] Mengeksekusi perintah: ${cmd}`);
        io.emit('serverToEsp', { cmd: cmd });
    });

    // F. Penanganan Disconnect
    socket.on('disconnect', () => {
        console.log(`[-] Client Disconnected: ${socket.id}`);
    });
});

// ==========================================================
// 7. BOOT SERVER
// ==========================================================
const PORT = 3000;
server.listen(PORT, () => {
    console.log(`\n=========================================`);
    console.log(`[SYSTEM] Middleware Broker Active (Port ${PORT})`);
    console.log(`[SYSTEM] Menunggu koneksi Node & Frontend...`);
    console.log(`=========================================\n`);
});