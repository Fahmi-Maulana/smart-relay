# ⚡ Smart Relay Protection

> **Sistem proteksi beban listrik otomatis** berbasis **ESP32 + PZEM-004T** dengan dashboard monitoring real-time melalui **WebSocket (Socket.IO)**. Server Node.js bertindak sebagai broker antara perangkat keras ESP32 dan antarmuka web, dilengkapi fitur **OTA (Over-the-Air) firmware update** dan **konfigurasi jaringan statis** jarak jauh.

---

## 📋 Daftar Isi

- [Fitur Utama](#-fitur-utama)
- [Arsitektur Sistem](#-arsitektur-sistem)
- [Komponen Hardware](#-komponen-hardware)
- [Logika Proteksi](#-logika-proteksi)
- [Struktur Folder](#-struktur-folder)
- [Prasyarat](#-prasyarat)
- [Cara Menjalankan Server](#-cara-menjalankan-server)
  - [Metode 1: Node.js Langsung](#metode-1-nodejs-langsung)
  - [Metode 2: Docker Compose (CasaOS)](#metode-2-docker-compose-casaos)
- [Konfigurasi Firmware ESP32](#-konfigurasi-firmware-esp32)
- [Dokumentasi Socket Events](#-dokumentasi-socket-events)
- [Dokumentasi REST API](#-dokumentasi-rest-api)
- [Fitur OTA Update](#-fitur-ota-update)
- [Fitur Konfigurasi Jaringan](#-fitur-konfigurasi-jaringan)
- [Dashboard Web](#-dashboard-web)
- [Troubleshooting](#-troubleshooting)

---

## ✨ Fitur Utama

| Fitur | Keterangan |
|---|---|
| ⚡ **Proteksi Tegangan** | Trip instan saat overvoltage (>231V) atau undervoltage (<198V) |
| 🔒 **Proteksi Beban** | Trip saat arus atau daya melebihi batas yang dikonfigurasi |
| 📊 **Filter DSP (EMA)** | Exponential Moving Average meredam *inrush current* adaptor laptop |
| 🔁 **Auto-Reset** | Relay menyala kembali otomatis setelah 60 detik cooldown jika beban aman |
| 📡 **Real-time WebSocket** | Data telemetri dikirim dari ESP32 ke dashboard setiap 300ms |
| 🌐 **OTA Firmware Update** | Upload firmware `.bin` via web dashboard tanpa kabel USB |
| 🔧 **Konfigurasi IP Statis** | Set IP dan gateway ESP32 dari jarak jauh via dashboard |
| 🌗 **Dark / Light Mode** | Dashboard mendukung tema gelap dan terang |
| 🐳 **Docker Ready** | Deploy mudah di CasaOS menggunakan Docker Compose |
| 📶 **WiFiManager** | Konfigurasi WiFi awal melalui Access Point `SmartRelay_AP` |

---

## 🏗️ Arsitektur Sistem

```
┌────────────────────────────────────────────────────────────────────────┐
│                        SMART RELAY SYSTEM                              │
│                                                                        │
│  ┌───────────────────┐   WebSocket (SSL)   ┌────────────────────────┐ │
│  │     ESP32         │ ──────────────────▶ │  Node.js Server        │ │
│  │  + PZEM-004T      │  42["espData", ...]  │  (Express + Socket.IO) │ │
│  │  + Relay          │                     │                        │ │
│  │  + LED WiFi       │ ◀────────────────── │  ✔ Broker data         │ │
│  │                   │  42["serverToEsp"]  │  ✔ File server (OTA)   │ │
│  │  Filter EMA (DSP) │                     │  ✔ REST upload API     │ │
│  │  Proteksi Beban   │                     └──────────┬─────────────┘ │
│  │  Auto-Reset 60dtk │                                │               │
│  └───────────────────┘                     ┌──────────▼─────────────┐ │
│                                            │  Web Dashboard         │ │
│  ┌───────────────────┐   Socket.IO         │  (public/index.html)   │ │
│  │  Browser / User   │ ◀────────────────── │                        │ │
│  │                   │  sensorData         │  ✔ Chart real-time     │ │
│  │  Kontrol Limit    │ ──────────────────▶ │  ✔ Kontrol relay       │ │
│  │  OTA Upload       │  setLimits          │  ✔ OTA upload          │ │
│  │  Konfigurasi IP   │  setNetwork         │  ✔ Konfigurasi jaringan│ │
│  └───────────────────┘  command            └────────────────────────┘ │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 🔌 Komponen Hardware

| Komponen | Keterangan | Pin ESP32 |
|---|---|---|
| **ESP32** | Mikrokontroler utama | — |
| **PZEM-004T v3.0** | Sensor tegangan, arus, daya, energi, frekuensi, PF | RX: GPIO16 / TX: GPIO17 |
| **Relay** | Saklar beban listrik (ON/OFF otomatis) | GPIO4 |
| **LED Indikator WiFi** | Menyala saat terhubung ke WiFi | GPIO2 |

### Wiring PZEM-004T ke ESP32

```
PZEM-004T          ESP32
---------          -----
  TX    ──────────▶ GPIO16 (RX2)
  RX    ◀────────── GPIO17 (TX2)
  GND   ──────────── GND
  VCC   ──────────── 5V
```

---

## 🛡️ Logika Proteksi

### Filter DSP — Exponential Moving Average (EMA)

Untuk mencegah trip palsu akibat *inrush current* (lonjakan arus saat perangkat baru dinyalakan), sistem menggunakan filter EMA dengan koefisien `α = 0.15`:

```
filteredI(t) = α × rawI(t) + (1 - α) × filteredI(t-1)
filteredP(t) = α × rawP(t) + (1 - α) × filteredP(t-1)
```

| Parameter | Nilai | Fungsi |
|---|---|---|
| `ALPHA` | `0.15` | Koefisien filter EMA (makin kecil = makin halus) |
| **Deadband Arus** | `< 0.02 A` → paksa 0 | Menghilangkan noise saat beban kosong |
| **Deadband Daya** | `< 0.5 W` → paksa 0 | Menghilangkan noise saat beban kosong |

### Kondisi Proteksi

| Kondisi | Trigger | Aksi |
|---|---|---|
| **Overvoltage** | `V > 231V` | Trip instan, masuk mode recovery |
| **Undervoltage** | `V < 198V` dan `V > 50V` | Trip instan, masuk mode recovery |
| **Overcurrent** | `rasioArus ≥ 1.05` (5% di atas batas) | Trip, masuk mode recovery |
| **Overpower** | `rasioDaya ≥ 1.05` (5% di atas batas) | Trip, masuk mode recovery |
| **Sensor Gagal** | 5× pembacaan NaN berturut-turut | Trip, tampilkan error sensor |

### Mekanisme Auto-Reset (Recovery)

```
Relay Trip → Cooldown 60 detik → Cek beban (< 95% batas?) → Nyala kembali ✅
                                                            → Perpanjang cooldown ❌
```

### Batas Default Awal

| Parameter | Nilai Default |
|---|---|
| Batas Arus | `2.0 A` |
| Batas Daya | `440 W` |

> Nilai batas disimpan persisten di **Flash ESP32** menggunakan library `Preferences`, sehingga tidak hilang saat restart.

---

## 📁 Struktur Folder

```
smart-relay/
├── 📄 server.js                     # Server broker Node.js (Express + Socket.IO)
├── 📄 package.json                  # Konfigurasi dependensi Node.js
├── 📄 docker-compose.yml            # Konfigurasi Docker untuk CasaOS
├── 📄 docker-compose.yml.bak        # Backup konfigurasi Docker lama
├── 📄 Smart_Relay_Protection.ino    # Firmware ESP32 (Arduino/PlatformIO)
└── 📂 public/
    ├── 📄 index.html                # Web dashboard (HTML + CSS + Socket.IO + Chart.js)
    └── 📄 firmware.bin              # File firmware OTA (di-upload melalui dashboard)
```

---

## 🛠️ Prasyarat

### Server (Node.js)
- **Node.js** versi `18` atau lebih baru
- **npm** (sudah termasuk dalam instalasi Node.js)

### Docker (Opsional)
- **Docker Engine** versi `20.10` atau lebih baru
- **Docker Compose** versi `v2` atau lebih baru

### Firmware ESP32
Library Arduino yang dibutuhkan (install via **Library Manager** di Arduino IDE):

| Library | Fungsi |
|---|---|
| `WiFiManager` | Konfigurasi WiFi via Access Point |
| `PZEM004Tv30` | Driver sensor PZEM-004T |
| `WebSocketsClient` | Koneksi WebSocket ke server |
| `ArduinoJson` | Parsing & serialisasi JSON |
| `HTTPClient` | HTTP request untuk OTA |
| `HTTPUpdate` | Eksekusi update firmware OTA |
| `Preferences` | Penyimpanan persisten di Flash |

---

## 🚀 Cara Menjalankan Server

### Metode 1: Node.js Langsung

```bash
# 1. Masuk ke direktori proyek
cd smart-relay

# 2. Install dependensi
npm install

# 3. Jalankan server
node server.js
```

Server akan berjalan di **http://localhost:3000**

---

### Metode 2: Docker Compose (CasaOS)

Metode ini direkomendasikan untuk deployment di CasaOS/server dengan data persisten di `/DATA/AppData/smart-relay`.

```bash
# 1. Masuk ke direktori proyek
cd smart-relay

# 2. Build dan jalankan container di background
docker compose up -d

# 3. Cek status container
docker compose ps

# 4. Lihat log real-time (opsional)
docker compose logs -f

# 5. Hentikan container
docker compose down
```

> **Port:** Server berjalan di **http://localhost:3000** (atau `http://<IP-SERVER>:3000`)

**Catatan volume:**  
Semua file proyek di-mount dari path host `/DATA/AppData/smart-relay` ke dalam container `/app`, sehingga file `firmware.bin` yang di-upload juga tersimpan persisten.

---

## ⚙️ Konfigurasi Firmware ESP32

Edit bagian berikut di awal file `Smart_Relay_Protection.ino` sebelum di-upload:

```cpp
// Alamat domain/IP server Node.js
const char* ws_host = "smart-relay.ijuloss.my.id";

// Port server
const int ws_port = 443;

// Gunakan SSL (true untuk HTTPS/WSS, false untuk HTTP/WS)
const bool use_ssl = true;
```

### Alur Konfigurasi WiFi Pertama Kali

```
1. Upload firmware ke ESP32
2. ESP32 akan membuat Access Point: "SmartRelay_AP"
3. Hubungkan HP/laptop ke WiFi "SmartRelay_AP"
4. Browser akan terbuka otomatis (atau buka http://192.168.4.1)
5. Pilih jaringan WiFi rumah dan masukkan password
6. ESP32 akan restart dan terhubung ke WiFi
7. LED GPIO2 menyala → koneksi berhasil ✅
```

---

## 📡 Dokumentasi Socket Events

Server berperan sebagai **broker (relay)** yang meneruskan pesan antara ESP32 dan browser.

### Events dari ESP32 → Server → Browser

| Event | Payload | Keterangan |
|---|---|---|
| `espData` | Objek telemetri lengkap | Data sensor real-time dari ESP32 |

**Contoh payload `espData`:**
```json
{
  "v": 220.5,
  "i": 1.23,
  "p": 270.6,
  "e": 0.185,
  "f": 50.0,
  "pf": 0.98,
  "relay": "ON",
  "alasan": "Kondisi Ideal",
  "rI": 0.615,
  "fzI": "Normal",
  "rP": 0.615,
  "fzP": "Normal",
  "fzV": "Normal",
  "recovery": false
}
```

| Field | Tipe | Keterangan |
|---|---|---|
| `v` | Float | Tegangan (Volt) |
| `i` | Float | Arus terfilter EMA (Ampere) |
| `p` | Float | Daya terfilter EMA (Watt) |
| `e` | Float | Energi akumulasi (kWh) |
| `f` | Float | Frekuensi (Hz) |
| `pf` | Float | Power Factor |
| `relay` | String | Status relay: `"ON"` / `"OFF"` |
| `alasan` | String | Keterangan kondisi sistem |
| `rI` | Float | Rasio arus (I / batasArus) |
| `fzI` | String | Himpunan arus: `"Normal"` / `"Overload"` |
| `rP` | Float | Rasio daya (P / batasDaya) |
| `fzP` | String | Himpunan daya: `"Normal"` / `"Overpower"` |
| `fzV` | String | Himpunan tegangan: `"Normal"` / `"Overvoltage"` / `"Undervoltage"` |
| `recovery` | Boolean | `true` jika sedang dalam mode cooldown |

---

### Events dari Browser → Server → ESP32

| Event | Payload | Keterangan |
|---|---|---|
| `setLimits` | `{ i: float, p: float }` | Update batas arus dan daya |
| `setNetwork` | `{ ip: string, gw: string }` | Konfigurasi IP statis ESP32 |
| `command` | String | Perintah langsung ke ESP32 |

**Daftar perintah (`command`):**

| Perintah | Fungsi |
|---|---|
| `resetRecovery` | Paksa reset mode cooldown, relay ON kembali |
| `resetKwh` | Reset akumulasi energi (kWh) ke 0 |
| `startOta` | Perintahkan ESP32 unduh & pasang firmware baru |
| `resetWifi` | Hapus data WiFi, ESP32 buat ulang Access Point |

---

### Events dari Server → Browser (sinkronisasi)

| Event | Payload | Keterangan |
|---|---|---|
| `sensorData` | Objek telemetri | Data sensor real-time (broadcast) |
| `updateLimits` | `{ i: float, p: float }` | Sinkronisasi limit ke semua tab yang terbuka |

---

## 📡 Dokumentasi REST API

### `POST /upload-ota`
Upload file firmware `.bin` untuk pembaruan OTA ke ESP32.

**Request:** `multipart/form-data` dengan field `firmware` berisi file `.bin`

**Contoh dengan `curl`:**
```bash
curl -X POST http://localhost:3000/upload-ota \
  -F "firmware=@/path/ke/Smart_Relay_Protection.ino.bin"
```

**Response:**
```
Deploy sukses! ESP32 sedang menarik firmware...
```

> Setelah upload berhasil, server otomatis mengirimkan event `startOta` ke ESP32 via Socket.IO.

---

### Static Files
Semua file di folder `public/` dapat diakses langsung:

| URL | File | Keterangan |
|---|---|---|
| `http://localhost:3000/` | `public/index.html` | Web Dashboard |
| `http://localhost:3000/firmware.bin` | `public/firmware.bin` | File firmware untuk ESP32 tarik via OTA |
| `http://localhost:3000/socket.io/socket.io.js` | *(auto)* | Library Socket.IO client |

---

## 🔄 Fitur OTA Update

Sistem mendukung pembaruan firmware ESP32 secara nirkabel tanpa koneksi USB.

**Prosedur OTA:**

```
1. Compile sketch Arduino → export file .bin
   (Arduino IDE: Sketch → Export Compiled Binary)

2. Buka dashboard web di browser

3. Pilih file firmware .bin di panel OTA Upload

4. Klik tombol [Deploy Firmware]

5. Server menyimpan file ke public/firmware.bin
   dan mengirim sinyal ke ESP32

6. ESP32 mengunduh firmware dari:
   https://<ws_host>:<ws_port>/firmware.bin

7. ESP32 melakukan update dan restart otomatis ✅
```

> ⚠️ Pastikan ukuran partisi flash ESP32 mencukupi untuk update OTA (minimal 4MB flash).

---

## 🌐 Fitur Konfigurasi Jaringan

IP statis ESP32 dapat diatur dari jarak jauh melalui dashboard tanpa perlu sentuh perangkat fisik.

**Prosedur:**
```
1. Buka panel "Konfigurasi Jaringan" di dashboard
2. Masukkan IP Statis (contoh: 192.168.1.100)
3. Masukkan IP Gateway (contoh: 192.168.1.1)
4. Klik [Simpan & Restart]
5. ESP32 menyimpan konfigurasi ke Flash (Preferences)
6. ESP32 restart dan menggunakan IP statis baru ✅
```

> Konfigurasi tersimpan persisten — tidak hilang meski ESP32 mati atau restart.

---

## 🖥️ Dashboard Web

Akses dashboard melalui browser:
- **Lokal:** `http://localhost:3000`
- **Jaringan:** `http://<IP-SERVER>:3000`
- **Domain (jika ada):** `https://smart-relay.ijuloss.my.id`

**Fitur dashboard:**

| Panel | Fungsi |
|---|---|
| **Status Koneksi** | Indikator koneksi WebSocket ke server dan ESP32 |
| **Telemetri Utama** | Tegangan (V), Arus (A), Daya (W), Energi (kWh), Frekuensi (Hz), PF |
| **Status Relay** | Tampilan ON/OFF + keterangan kondisi sistem |
| **Rasio Beban** | Progress bar rasio arus dan daya terhadap batas |
| **Himpunan Fuzzy** | Label status: Normal / Overload / Overvoltage / dll |
| **Chart Real-time** | Grafik arus dan daya yang diperbarui secara live |
| **Set Batas** | Input untuk mengubah batas arus (A) dan daya (W) |
| **Kontrol Relay** | Tombol Reset Recovery, Reset kWh, Reset WiFi |
| **OTA Upload** | Form upload firmware `.bin` dan trigger update |
| **Konfigurasi IP** | Form set IP statis dan gateway ESP32 |
| **Dark / Light Mode** | Toggle tema tampilan |

---

## 🔧 Troubleshooting

| Masalah | Kemungkinan Penyebab | Solusi |
|---|---|---|
| ESP32 tidak konek ke server | URL WebSocket salah / server mati | Periksa `ws_host` dan `ws_port` di firmware; pastikan server aktif |
| Dashboard tidak menerima data | ESP32 offline atau socket belum `ready` | Cek log server; tunggu `socketIoReady = true` |
| Relay trip terus (false trip) | Inrush current tinggi | Kurangi nilai `ALPHA` pada filter EMA di firmware |
| OTA gagal | File `.bin` salah / URL tidak bisa diakses | Pastikan server bisa diakses dari ESP32; cek log serial |
| LED WiFi tidak menyala | Gagal konek WiFi | Reset WiFi dengan perintah `resetWifi` dari dashboard, lalu konfigurasi ulang |
| Nilai sensor selalu NaN | PZEM tidak terhubung / wiring salah | Periksa wiring TX/RX dan catu daya PZEM |
| Container Docker restart loop | Port 3000 sudah dipakai | Ganti port di `docker-compose.yml` |
| IP statis tidak berfungsi | Gateway tidak valid | Pastikan gateway berada di subnet yang sama dengan IP statis |

---

## 📦 Dependensi

### Server (Node.js)

| Package | Versi | Fungsi |
|---|---|---|
| [express](https://expressjs.com/) | `^4.18.2` | HTTP server, routing, static files |
| [socket.io](https://socket.io/) | `^4.7.2` | WebSocket broker real-time (dengan fallback polling) |
| [multer](https://github.com/expressjs/multer) | `^1.4.5-lts.1` | Middleware upload file (OTA firmware) |

### Dashboard (CDN)

| Library | Fungsi |
|---|---|
| [Socket.IO Client](https://socket.io/) | Koneksi WebSocket dari browser |
| [Chart.js](https://www.chartjs.org/) | Grafik real-time arus dan daya |
| [Plus Jakarta Sans](https://fonts.google.com/specimen/Plus+Jakarta+Sans) | Tipografi dashboard |
| [JetBrains Mono](https://fonts.google.com/specimen/JetBrains+Mono) | Font monospace untuk nilai numerik |

---

## 📝 Lisensi

Proyek ini dibuat untuk keperluan **penelitian dan pengembangan** sistem proteksi beban listrik cerdas berbasis IoT. Bebas digunakan dan dimodifikasi untuk keperluan pendidikan dan riset.

---

<div align="center">
  <sub>Dibuat dengan ❤️ untuk sistem Smart Home IoT &nbsp;·&nbsp; ESP32 + PZEM-004T + Node.js + Socket.IO + Docker</sub>
</div>
