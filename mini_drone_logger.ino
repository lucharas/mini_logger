#include <WiFi.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_BME280.h> 

// --- KONFIGURACJA SPRZĘTOWA (GPIO) ---
const int PIN_LED = 7;        // builtin LED (GPIO 7)
const int PIN_BUTTON = 2;     // Przycisk (GPIO 2, LOW = wciśnięty)
const int PIN_ADC = 0;        // Testowy ADC (GPIO 0)
const int SDA_PIN = 8;        // I2C SDA (GPIO 8)
const int SCL_PIN = 9;        // I2C SCL (GPIO 9)

// --- KONFIGURACJA SYSTEMOWA ---
#define LOG_FILENAME "/data_log.csv"
const size_t MAX_LOG_SIZE_BYTES = 200 * 1024; // Limit 200 kB

// --- KONFIGURACJA BUFOROWANIA I CZASU ---
const size_t BUFFER_SIZE = 1024; // 1 kB
char logBuffer[BUFFER_SIZE];
size_t bufferIndex = 0;

unsigned long lastFlushTime = 0;
const unsigned long FLUSH_INTERVAL_MS = 10000; // 10 sekund
unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL_MS = 1000;    // 1 sekunda (zgodnie z 26 B/s)

unsigned long idleStartTime = 0;
const unsigned long LOG_DELAY_MS = 30000; // 30 sekund IDLE

unsigned long apStartTime = 0;
const unsigned long AP_TIMEOUT_MS = 10 * 60 * 1000; // 10 minut AP

unsigned long logStartTime = 0; // Czas startu stanu LOG (dla t_s)
size_t currentLogSize = 0; // Bieżący rozmiar logu

// --- KONFIGURACJA WIFI (AP) ---
const char* ap_ssid = "MINI_LOGGER";
const char* ap_password = "";             // Brak hasła

// --- MASZYNA STANÓW ---
enum State {
    IDLE,
    LOG,
    SAFE_EXIT,
    AP
};
State currentState = IDLE;

WebServer server(80);
Adafruit_BME280 bme; // Obiekt dla czujnika BME280

// --- NAGŁÓWKI FUNKCJI STANÓW ---
void enterIDLE();
void loopIDLE();
void enterLOG();
void loopLOG();
void enterSAFE_EXIT();
void loopSAFE_EXIT();
void enterAP();
void loopAP();
void changeState(State newState);

// --- FUNKCJE POMOCNICZE ---

// Inicjalizacja I2C i BME280
void setupI2C() {
    Wire.begin(SDA_PIN, SCL_PIN);
    if (!bme.begin(0x76)) {
        bme.begin(0x77); // Próba adresu alternatywnego
    }
}

// Funkcja dodająca dane do bufora
void bufferData(const char* data) {
    size_t len = strlen(data);
    // Sprawdzenie, czy bufor zmieści nowe dane + znak terminujący
    if (bufferIndex + len < BUFFER_SIZE) {
        memcpy(logBuffer + bufferIndex, data, len);
        bufferIndex += len;
    } 
}

// Ostatni zrzut bufora na SPIFFS (flush) i unmount
void finalFlushAndUnmount() {
    if (bufferIndex > 0) {
        File file = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
        if (file) {
            file.write((uint8_t*)logBuffer, bufferIndex);
            file.flush();
            file.close();
        }
        bufferIndex = 0;
    }

    // Odmontowanie SPIFFS
    SPIFFS.end();
}

// --- LOGIKA WIFI SERVER (AP) (Bez zmian) ---

void handleRoot() {
    String html = "<html><head><title>Mini Logger</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial;text-align:center;margin-top:50px;} button{padding:10px;margin:5px;font-size:16px;}</style>";
    html += "</head><body>";
    html += "<h2>Mini Logger AP Mode</h2>";
    html += "Rozmiar Logu: ";
    if (SPIFFS.exists(LOG_FILENAME)) {
        File file = SPIFFS.open(LOG_FILENAME, FILE_READ);
        html += String(file.size() / 1024.0, 2) + " kB";
        file.close();
    } else {
        html += "0.00 kB (Brak pliku)";
    }
    html += "<br><br>";
    html += "<button onclick=\"location.href='/download'\">DOWNLOAD</button><br>";
    html += "<button onclick=\"location.href='/erase'\">ERASE</button><br>";
    html += "<button onclick=\"location.href='/idle'\">GOTO IDLE</button>";
    html += "<p>Timeout za 10 minut.</p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handleDownload() {
    if (SPIFFS.exists(LOG_FILENAME)) {
        File file = SPIFFS.open(LOG_FILENAME, FILE_READ);
        server.streamFile(file, "text/csv");
        file.close();
    } else {
        server.send(404, "text/plain", "Brak pliku logu!");
    }
}

void handleErase() {
    if (SPIFFS.exists(LOG_FILENAME)) {
        SPIFFS.remove(LOG_FILENAME);
        server.send(200, "text/plain", "Plik logu skasowany.");
    } else {
        server.send(200, "text/plain", "Brak pliku do kasowania.");
    }
}

void handleGotoIdle() {
    server.send(200, "text/plain", "Przechodzę do IDLE. Rozłącz się.");
    delay(500);
    changeState(IDLE);
}

// --- ZARZĄDZANIE STANAMI ---

void changeState(State newState) {
    currentState = newState;
    switch (currentState) {
        case IDLE:
            enterIDLE();
            break;
        case LOG:
            enterLOG();
            break;
        case SAFE_EXIT:
            enterSAFE_EXIT();
            break;
        case AP:
            enterAP();
            break;
    }
}

// ----------------------------------------------------
// IMPLEMENTACJA STANÓW
// ----------------------------------------------------

// STAN 1: IDLE
void enterIDLE() {
    digitalWrite(PIN_LED, HIGH); // LED ON
    WiFi.mode(WIFI_OFF);
    idleStartTime = millis();
}

void loopIDLE() {
    // Przejście do LOG po 30 sekundach
    if (millis() - idleStartTime >= LOG_DELAY_MS) {
        changeState(LOG);
    }
}

// STAN 2: LOG
void enterLOG() {
    // Zapis czasu startu LOG
    logStartTime = millis(); 

    // Inicjalizacja SPIFFS
    if (!SPIFFS.begin(true)) {
        changeState(AP); 
        return;
    }

    // Inicjalizacja nagłówka pliku (jeśli plik nie istnieje)
    if (!SPIFFS.exists(LOG_FILENAME)) {
        File file = SPIFFS.open(LOG_FILENAME, FILE_WRITE);
        file.println("t_s,T_ntc,T_bme,RH,P_hPa");
        currentLogSize = file.size();
        file.close();
    } else {
        File file = SPIFFS.open(LOG_FILENAME, FILE_READ);
        currentLogSize = file.size();
        file.close();
    }

    lastFlushTime = millis();
    lastLogTime = millis();
    bufferIndex = 0;
}

void loopLOG() {
    // 1. Zbieranie telemetrii (co 1 sekundę)
    if (millis() - lastLogTime >= LOG_INTERVAL_MS) {
        // Obliczanie czasu t_s
        long t_s = (millis() - logStartTime) / 1000;

        // Odczyt T_ntc (ADC)
        int t_ntc_adc = analogRead(PIN_ADC);

        // Odczyt z BME280
        float T_bme = bme.readTemperature();
        int RH = (int)round(bme.readHumidity());
        float P_hPa = bme.readPressure() / 100.0F; // Przeliczenie Pa na hPa

        // Formatowanie danych CSV: t_s, T_ntc, T_bme, RH, P_hPa
        String logEntry = String(t_s) + "," +
                          String(t_ntc_adc) + "," +
                          String(T_bme, 1) + "," + // T_bme zaokrąglone do 1 miejsca
                          String(RH) + "," +
                          String(P_hPa, 2) + "\n";
        
        bufferData(logEntry.c_str());
        lastLogTime = millis();
    }

    // 2. Warunek PRZEJŚCIA (BUTTON)
    if (digitalRead(PIN_BUTTON) == LOW) {
        changeState(SAFE_EXIT);
        return;
    }

    // 3. Warunek PRZEJŚCIA (GUARD: 200 kB)
    if (currentLogSize > MAX_LOG_SIZE_BYTES) {
        changeState(SAFE_EXIT);
        return;
    }

    // 4. Warunek ZAPISU (FLUSH 10s)
    if (millis() - lastFlushTime >= FLUSH_INTERVAL_MS) {
        if (bufferIndex > 0) {
            File logFile = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
            if (logFile) {
                logFile.write((uint8_t*)logBuffer, bufferIndex);
                logFile.flush();
                currentLogSize = logFile.size(); // Aktualizacja rozmiaru
                logFile.close();
                bufferIndex = 0;
            }
        }
        lastFlushTime = millis();
    }

    // 5. Miganie LED (1 Hz - 900 ms ON, 100 ms OFF)
    if ((millis() % 1000) < 900) {
        digitalWrite(PIN_LED, LOW); 
    } else {
        digitalWrite(PIN_LED, HIGH); 
    }
}

// STAN 3: SAFE_EXIT
void enterSAFE_EXIT() {
    finalFlushAndUnmount();
    changeState(AP);
}

void loopSAFE_EXIT() {
}

// STAN 4: AP
void enterAP() {
    // Mount SPIFFS for reading logs
    SPIFFS.begin();

    WiFi.softAP(ap_ssid, ap_password);

    server.on("/", handleRoot);
    server.on("/download", handleDownload);
    server.on("/erase", handleErase);
    server.on("/idle", handleGotoIdle);
    server.begin();

    apStartTime = millis();
}

void loopAP() {
    server.handleClient();

    // 1. Warunek PRZEJŚCIA (TIMEOUT 10 min)
    if (millis() - apStartTime >= AP_TIMEOUT_MS) {
        changeState(IDLE);
        return;
    }

    // 2. Miganie LED (5 Hz - co 100 ms)
    if ((millis() / 100) % 2 == 0) {
        digitalWrite(PIN_LED, LOW); 
    } else {
        digitalWrite(PIN_LED, HIGH); 
    }
}

// ----------------------------------------------------
// GŁÓWNA PĘTLA ARDUINO
// ----------------------------------------------------

void setup() {

    // Konfiguracja GPIO
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_ADC, INPUT);
    setupI2C(); // Inicjalizacja I2C i BME280

    changeState(IDLE);
}

void loop() {
    switch (currentState) {
        case IDLE:
            loopIDLE();
            break;
        case LOG:
            loopLOG();
            break;
        case SAFE_EXIT:
            loopSAFE_EXIT();
            break;
        case AP:
            loopAP();
            break;
    }
}