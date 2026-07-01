#include <WiFi.h>
#include <WiFiManager.h>
#include <PZEM004Tv30.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

/// =================================================================
// KONFIGURASI SERVER CASAOS
// =================================================================
const char* ws_host = "smart-relay.ijuloss.my.id"; 
const int ws_port = 443;
const bool use_ssl = true;            

#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17
#define RELAY_PIN 4
#define LED_WIFI 2

PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);
WebSocketsClient webSocket;
Preferences preferences;
WiFiManager wm;

// STATE VARIABLES & TELEMETRI
float batasArus = 2.0, batasDaya = 440.0;
float currentV = 0.0, currentI = 0.0, currentP = 0.0, currentE = 0.0, currentF = 0.0, currentPF = 0.0;
float rasioArus = 0.0, rasioDaya = 0.0;

// KOEFISIEN FILTER DSP EMA (Exponential Moving Average)
// Semakin kecil nilai ALPHA, semakin kuat meredam inrush current adaptor laptop
const float ALPHA = 0.15; 
float filteredI = 0.0;
float filteredP = 0.0;

String relayStatus = "OFF", alasanFuzzy = "Menunggu Server...";
String himpunanTegangan = "-", himpunanArus = "-", himpunanDaya = "-";

unsigned long lastReadTime = 0;
const unsigned long readInterval = 300; 
unsigned long lastTripTime = 0;
const unsigned long recoveryInterval = 60000; 

bool isRecoveryMode = false;
bool socketIoReady = false; 

bool pendingRestart = false;
unsigned long restartTime = 0;

void setRelay(bool stateON, String alasan) {
    digitalWrite(RELAY_PIN, stateON ? HIGH : LOW);
    relayStatus = stateON ? "ON" : "OFF";
    alasanFuzzy = alasan;
}

// =================================================================
// LOGIKA PROTEKSI DIGITAL MENGGUNAKAN FILTER EMA (TANPA TIMER)
// =================================================================
void jalankanFuzzyLogic() {
    float rawV = pzem.voltage(); 
    rawV = rawV + 2.0;
    float rawI = pzem.current(); 
    float rawP = pzem.power();
    float e = pzem.energy(); 
    float f = pzem.frequency(); 
    float pf = pzem.pf();

    static int pzemFail = 0;
    if (isnan(rawV) || isnan(rawI)) {
        pzemFail++;
        if (pzemFail >= 5) {
            himpunanTegangan = "Sensor Gagal"; himpunanArus = "Sensor Gagal"; himpunanDaya = "Sensor Gagal";
            setRelay(false, "Trip: Sensor PZEM Terputus/Rusak!");
        }
        return; 
    } 
    
    pzemFail = 0; 
    currentV = rawV; currentE = e; currentF = f; currentPF = pf;

    // PROSES FILTERISASI DIGITAL (Meredam entakan inrush current secara matematis)
    if (filteredI == 0.0 && rawI > 0.0) {
        filteredI = rawI; // Inisialisasi awal agar tidak butuh waktu merangkak dari nol saat start
        filteredP = rawP;
    } else {
        filteredI = (ALPHA * rawI) + ((1.0 - ALPHA) * filteredI);
        filteredP = (ALPHA * rawP) + ((1.0 - ALPHA) * filteredP);
    }

    // DEADBAND: Jika nilai filter sudah sangat kecil, paksa menjadi 0 mutlak
    if (filteredI < 0.02) filteredI = 0.0;
    if (filteredP < 0.5) filteredP = 0.0;

    // Gunakan data hasil filter untuk pelaporan telemetri utama dan evaluasi beban
    currentI = filteredI;
    currentP = filteredP;

    rasioArus = currentI / batasArus;
    rasioDaya = currentP / batasDaya;

    // Evaluasi Tegangan WAJIB INSTAN (Menggunakan data mentah demi aspek keselamatan)
    himpunanTegangan = (currentV > 231.0) ? "Overvoltage" : (currentV < 198.0 && currentV > 50.0) ? "Undervoltage" : "Normal";
    himpunanArus = (rasioArus >= 1.0) ? "Overload" : "Normal";
    himpunanDaya = (rasioDaya >= 1.0) ? "Overpower" : "Normal";

    // 1. Eksekusi Proteksi Tegangan Instan
    if (himpunanTegangan != "Normal") {
        setRelay(false, "Trip Kritis: " + himpunanTegangan);
        isRecoveryMode = true; 
        lastTripTime = millis();
        return;
    } 

    // 2. Evaluasi Kondisi Beban Berbasis Nilai Filter (Histeresis Kompleks Tanpa Penahan Waktu)
    if (!isRecoveryMode) {
        // Toleransi Histeresis 5% di atas batas nominal
        if (rasioArus >= 1.05 || rasioDaya >= 1.05) { 
            setRelay(false, "Trip Proteksi Beban: " + String(rasioArus >= 1.05 ? "Arus" : "Daya") + " Berlebih");
            isRecoveryMode = true; 
            lastTripTime = millis();
        } 
        else {
            if (relayStatus == "OFF") {
                setRelay(true, "Kondisi Ideal");
            } else {
                alasanFuzzy = "Kondisi Ideal";
            }
        }
    } 
    // 3. Mekanisme Pemulihan Beban (Non-Blocking State Cooldown)
    else {
        long sisa = (recoveryInterval - (millis() - lastTripTime)) / 1000;
        if (sisa < 0) sisa = 0;
        alasanFuzzy = "Pendinginan Sistem... (" + String(sisa) + " dtk)";
        
        if (millis() - lastTripTime >= recoveryInterval) {
            // Syarat menyala kembali: beban hasil filter harus benar-benar turun ke zona aman (95%)
            if (rasioArus < 0.95 && rasioDaya < 0.95) { 
                setRelay(true, "Auto-Reset: Sistem Normal");
                isRecoveryMode = false; 
                filteredI = 0.0; // Reset filter saat beban dinyalakan kembali untuk deteksi transien baru
                filteredP = 0.0;
            } else {
                lastTripTime = millis(); 
                Serial.println("[FUZZY] Pemulihan ditunda, beban terfilter masih tinggi.");
            }
        }
    }
}

void jalankanOTA() {
    Serial.println("\n[OTA] Mengunduh Firmware...");
    setRelay(false, "Menerapkan Update Firmware..."); 
    String otaURL = String(use_ssl ? "https://" : "http://") + ws_host + ":" + ws_port + "/firmware.bin";
    t_httpUpdate_return ret;

    if (use_ssl) {
        WiFiClientSecure clientS; clientS.setInsecure();
        ret = httpUpdate.update(clientS, otaURL);
    } else {
        WiFiClient client; ret = httpUpdate.update(client, otaURL);
    }
    if (ret == HTTP_UPDATE_OK) {
        pendingRestart = true;
        restartTime = millis();
    }
}

void triggerRestart() {
    pendingRestart = true;
    restartTime = millis();
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_DISCONNECTED) { socketIoReady = false; } 
    else if (type == WStype_TEXT) {
        String msg = (char*)payload;
        if (msg.startsWith("0")) { webSocket.sendTXT("40"); socketIoReady = true; return; }
        if (msg == "2") { webSocket.sendTXT("3"); return; }

        if (msg.startsWith("42")) {
            msg.remove(0, 2); JsonDocument doc;
            if (!deserializeJson(doc, msg)) {
                if (doc[0].as<String>() == "serverToEsp") {
                    String cmd = doc[1]["cmd"].as<String>();
                    if (cmd == "applyLimits") {
                        batasArus = doc[1]["data"]["i"].as<float>();
                        batasDaya = doc[1]["data"]["p"].as<float>();
                        preferences.putFloat("batasArus", batasArus); preferences.putFloat("batasDaya", batasDaya);
                    } 
                    else if (cmd == "applyNetwork") {
                        preferences.putString("statIp", doc[1]["data"]["ip"].as<String>());
                        preferences.putString("statGw", doc[1]["data"]["gw"].as<String>());
                        Serial.println("[NETWORK] Konfigurasi Baru Disimpan. Memicu Restart...");
                        triggerRestart();
                    }
                    else if (cmd == "resetRecovery") { isRecoveryMode = false; setRelay(true, "Paksa Reset Web"); filteredI = 0.0; filteredP = 0.0; }
                    else if (cmd == "resetKwh") { pzem.resetEnergy(); currentE = 0.0; }
                    else if (cmd == "startOta") { jalankanOTA(); }
                    else if (cmd == "resetWifi") { 
                        wm.resetSettings(); 
                        preferences.remove("statIp"); 
                        Serial.println("[NETWORK] Memori Jaringan Dibersihkan. Memicu Restart...");
                        triggerRestart();
                    }
                }
            }
        }
    }
}

void kirimDataKeServer() {
    if (!socketIoReady) return;
    JsonDocument doc;
    doc["v"] = currentV; doc["i"] = currentI; doc["p"] = currentP;
    doc["e"] = currentE; doc["f"] = currentF; doc["pf"] = currentPF;
    doc["relay"] = relayStatus; doc["alasan"] = alasanFuzzy;
    doc["rI"] = rasioArus; doc["fzI"] = himpunanArus;
    doc["rP"] = rasioDaya; doc["fzP"] = himpunanDaya;
    doc["fzV"] = himpunanTegangan; doc["recovery"] = isRecoveryMode;

    String jsonData; serializeJson(doc, jsonData);
    webSocket.sendTXT("42[\"espData\"," + jsonData + "]");
}

void setup() {
    Serial.begin(115200);
    pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW); 
    pinMode(LED_WIFI, OUTPUT);

    preferences.begin("relay-app", false);
    batasArus = preferences.getFloat("batasArus", 2.0); 
    batasDaya = preferences.getFloat("batasDaya", 440.0);

    String statIp = preferences.getString("statIp", "");
    String statGw = preferences.getString("statGw", "");
    if (statIp != "") {
        IPAddress ip, gw, sn(255, 255, 255, 0); 
        ip.fromString(statIp);
        if (statGw != "") { gw.fromString(statGw); } 
        else { gw = ip; gw[3] = 1; }
        wm.setSTAStaticIPConfig(ip, gw, sn);
    }

    wm.autoConnect("SmartRelay_AP"); 
    digitalWrite(LED_WIFI, HIGH);

    String socketIoUrl = "/socket.io/?EIO=4&transport=websocket";
    if (use_ssl) webSocket.beginSSL(ws_host, ws_port, socketIoUrl);
    else webSocket.begin(ws_host, ws_port, socketIoUrl);
    
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000); 
    setRelay(true, "Sistem Pengawasan Dimulai");
}

void loop() {
    // Penanganan Restart Tertunda Tanpa Fungsi Blocking Delay
    if (pendingRestart && (millis() - restartTime >= 1000)) {
        ESP.restart();
    }

    webSocket.loop();
    if (millis() - lastReadTime >= readInterval) {
        lastReadTime = millis();
        jalankanFuzzyLogic();
        if (WiFi.status() == WL_CONNECTED) { kirimDataKeServer(); }
    }
}