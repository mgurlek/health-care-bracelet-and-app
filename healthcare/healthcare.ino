#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include "MAX30100_PulseOximeter.h"
#include <MPU6050_tockn.h> // MPU6050 Kütüphanesi
#include "time.h"          // Zaman (Timestamp) için

// --- 1. AĞ AYARLARI ---
#define WIFI_SSID "Mert"
#define WIFI_PASSWORD "11223344"

// --- 2. FIREBASE ADRESİ ---
String FIREBASE_URL = "https://healthcare-rmys-default-rtdb.europe-west1.firebasedatabase.app/users/Foi9NDLcQpSaQ5PPPCTxeOrRknk1/patient/current.json";

#define BUTON_PIN 15 

// --- NTP ZAMAN AYARLARI (TÜRKİYE) ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 10800; // UTC +3 (3 saat * 3600 saniye)
const int   daylightOffset_sec = 0;

// Global Değişkenler (Core 0 ve Core 1 arası iletişim)
volatile int guncelNabiz = 0;
volatile int guncelSpO2 = 0;
volatile float ax = 0, ay = 0, az = 0; // İvme verileri
volatile uint32_t sonVurusZamani = 0;
volatile bool acilGonderimGerekli = false; 

PulseOximeter pox;
MPU6050 mpu6050(Wire);
TaskHandle_t SensorGorevi;

// Kalp Atışı Callback
void onBeatDetected() {
    // Serial.println("♥"); // Serial'i çok meşgul etmemek için kapattım
    sonVurusZamani = millis();
}

// Zaman Damgası (Epoch Time) Alma Fonksiyonu
unsigned long long getEpochTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return 0;
    }
    time_t now;
    time(&now);
    return (unsigned long long)now * 1000; // Milisaniye cinsinden
}

// --- SENSÖR ÇEKİRDEĞİ (CORE 0) ---
// Sensörleri okur ve global değişkenlere yazar
void SensorCode(void * pvParameters) {
    Wire.begin(21, 22);
    Wire.setClock(100000);
    
    // 1. MPU6050 Başlat
    mpu6050.begin();
    // mpu6050.calcGyroOffsets(true); // İstersen açabilirsin (ilk açılışta 3 sn bekletir)
    
    // 2. MAX30100 Başlat
    if (!pox.begin()) {
        Serial.println("HATA: MAX30100 Baslatilamadi!");
    } else {
        pox.setOnBeatDetectedCallback(onBeatDetected);
        pox.setIRLedCurrent(MAX30100_LED_CURR_50MA);
    }

    for(;;) {
        // --- Sensör Güncellemeleri ---
        pox.update();
        mpu6050.update();

        // --- Verileri Globale Aktar ---
        // Nabız (Parmak kontrolü ile)
        if (millis() - sonVurusZamani < 3000) {
            guncelNabiz = (int)pox.getHeartRate();
            guncelSpO2 = pox.getSpO2();
        } else {
            guncelNabiz = 0;
            guncelSpO2 = 0;
        }

        // İvme (X, Y, Z)
        ax = mpu6050.getAccX();
        ay = mpu6050.getAccY();
        az = mpu6050.getAccZ();

        // --- Hızlı Buton Kontrolü ---
        if (digitalRead(BUTON_PIN) == LOW) {
            acilGonderimGerekli = true; // Core 1'i uyandır
        }
        
        vTaskDelay(1); 
    }
}

// --- ANA KURULUM (CORE 1) ---
void setup() {
    Serial.begin(115200);
    delay(1000);

    // 1. WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("WiFi Baglaniyor");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
    }
    Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());
    
    // 2. Zaman Ayarı (NTP)
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    pinMode(BUTON_PIN, INPUT_PULLUP);

    // 3. Sensör Görevini Başlat (Core 0)
    xTaskCreatePinnedToCore(SensorCode, "SensorTask", 10000, NULL, 1, &SensorGorevi, 0);
    Serial.println("Tum Sistem (MPU + MAX + WiFi) Aktif!");
}

void loop() {
    static uint32_t sonNormalGonderim = 0;

    // Gönderim Şartı: 1 saniye dolduysa VEYA butona basıldıysa
    if ((millis() - sonNormalGonderim > 1000) || (acilGonderimGerekli == true)) {
        
        bool butonDurumu = false;
        if (acilGonderimGerekli) {
            Serial.println(">>> BUTON TETIKLEDI! <<<");
            butonDurumu = true;
            acilGonderimGerekli = false; 
        }
        
        sonNormalGonderim = millis();

        // Verileri Çek
        int nabiz = guncelNabiz;
        int spo2 = guncelSpO2;
        float x = ax;
        float y = ay;
        float z = az;
        unsigned long long zaman = getEpochTime();

        // Seri Porta Yaz (Kontrol için)
        Serial.printf("Nabiz: %d | X: %.2f Y: %.2f Z: %.2f | Zaman: %llu\n", nabiz, x, y, z, zaman);

        // --- JSON PAKETİ (İç İçe Yapı) ---
        // Şuna benzer bir yapı oluşturuyoruz:
        // { 
        //    "heart_rate": 75, 
        //    "spo2": 98, 
        //    "button_pressed": false,
        //    "timestamp": 1700500...,
        //    "accel": { "x": 0.1, "y": 0.05, "z": 0.98 }
        // }
        
        String jsonPaketi = "{";
        jsonPaketi += "\"heart_rate\":" + String(nabiz) + ",";
        jsonPaketi += "\"spo2\":" + String(spo2) + ",";
        jsonPaketi += "\"button_pressed\":" + String(butonDurumu ? "true" : "false") + ",";
        jsonPaketi += "\"timestamp\":" + String((unsigned long)zaman) + ","; // Long Long string çevrimi bazen sorunludur, cast ettik
        
        jsonPaketi += "\"accel\":{";
        jsonPaketi += "\"x\":" + String(x) + ",";
        jsonPaketi += "\"y\":" + String(y) + ",";
        jsonPaketi += "\"z\":" + String(z);
        jsonPaketi += "}"; // Accel kapa
        
        jsonPaketi += "}"; // Ana paketi kapa

        // GÖNDER
        if(WiFi.status() == WL_CONNECTED){
            HTTPClient http;
            http.begin(FIREBASE_URL);
            http.addHeader("Content-Type", "application/json");

            int kod = http.PUT(jsonPaketi);
            if(kod == 200) Serial.println("Firebase Guncellendi ✅");
            else Serial.println("Hata Kodu: " + String(kod));
            
            http.end();
        }
    }
}