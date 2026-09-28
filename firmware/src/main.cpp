// Carlos Sierra

#include <M5Unified.h>
#include <DHT.h>
#include <stdarg.h>

constexpr uint8_t LED_PIN = 39;                 // G39 on the AtomS3R header
constexpr uint8_t BUTTON_PIN = 41;              // Screen button on the AtomS3R (active LOW)
constexpr uint8_t DHT_PIN = 38;                 // DHT22 data on G38
constexpr uint32_t BLINK_INTERVAL_QUICK = 500;  // ms on, then ms off
constexpr uint32_t BLINK_INTERVAL_SLOW = 2000;  // ms on, then ms off
constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t STATUS_INTERVAL = 1000;      // ms between serial status lines
constexpr uint32_t DHT_INTERVAL = 2000;         // DHT22 can't be read faster than every 2 s
constexpr int HEADER_H = 20;                    // top bar showing the current mode

DHT dht(DHT_PIN, DHT22);
M5Canvas canvas(&M5.Display);  // off-screen buffer: draw here, then push in one go (no flicker)

uint32_t lastStatus = 0;
uint32_t lastDhtRead = 0;

bool quickMode = true;
bool ledOn = false;
uint32_t lastToggle = 0;

float temperature = NAN;
float humidity = NAN;

// Rolling record of the last 100 DHT scans (true = NaN / failed)
constexpr int HISTORY_SIZE = 100;
bool failHistory[HISTORY_SIZE] = {};
int historyCount = 0;   // scans recorded so far, up to HISTORY_SIZE
int historyIndex = 0;   // next slot to overwrite
int failCount = 0;      // failed scans currently in the window

void recordScan(bool failed) {
    if (historyCount == HISTORY_SIZE) {
        if (failHistory[historyIndex]) failCount--;  // drop the oldest scan
    } else {
        historyCount++;
    }
    failHistory[historyIndex] = failed;
    if (failed) failCount++;
    historyIndex = (historyIndex + 1) % HISTORY_SIZE;
}

int nanPercent() {
    return historyCount ? (failCount * 100 + historyCount / 2) / historyCount : 0;
}

int lastButtonReading = HIGH;
int buttonState = HIGH;
uint32_t lastDebounceTime = 0;

// Sends "[HH:MM:SS.mmm] <message>" to Serial
void logLine(const char* fmt, ...) {
    char msg[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    uint32_t ms = millis();
    uint32_t totalSec = ms / 1000;
    Serial.printf("[%02lu:%02lu:%02lu.%03lu] %s\n",
                  (unsigned long)(totalSec / 3600),
                  (unsigned long)((totalSec / 60) % 60),
                  (unsigned long)(totalSec % 60),
                  (unsigned long)(ms % 1000),
                  msg);
}

void printStatus() {
    logLine("State: %s, LED: %s", quickMode ? "Quick" : "Slow", ledOn ? "ON" : "OFF");
}

// Draws the whole screen into the canvas, then pushes it to the display at once
void drawScreen() {
    const int w = canvas.width();
    const int cx = w / 2;
    char buf[16];

    canvas.fillScreen(TFT_BLACK);

    // Header: % of NaN readings over the last scans
    int pct = nanPercent();
    uint16_t headerColor = pct == 0 ? TFT_DARKGREEN : (pct <= 10 ? TFT_BROWN : TFT_MAROON);
    canvas.fillRect(0, 0, w, HEADER_H, headerColor);
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(TFT_WHITE);
    if (historyCount == 0) strcpy(buf, "NaN: --");
    else snprintf(buf, sizeof(buf), "NaN %d%% / %d", pct, historyCount);
    canvas.drawString(buf, cx, HEADER_H / 2);

    // Temperature
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(isnan(temperature) ? TFT_RED : TFT_ORANGE);
    if (isnan(temperature)) strcpy(buf, "NaN C");
    else snprintf(buf, sizeof(buf), "%.1f C", temperature);
    canvas.drawString(buf, cx, 44);

    // Humidity
    canvas.setTextColor(isnan(humidity) ? TFT_RED : TFT_CYAN);
    if (isnan(humidity)) strcpy(buf, "NaN %");
    else snprintf(buf, sizeof(buf), "%.1f %%", humidity);
    canvas.drawString(buf, cx, 76);

    // LED state: indicator dot + ON/OFF
    const int ledY = 108;
    if (ledOn) canvas.fillCircle(cx - 30, ledY, 10, TFT_RED);
    else canvas.drawCircle(cx - 30, ledY, 10, TFT_DARKGREY);
    canvas.setTextColor(ledOn ? TFT_WHITE : TFT_DARKGREY);
    canvas.drawString(ledOn ? "ON" : "OFF", cx + 14, ledY);

    canvas.pushSprite(0, 0);
}

void readDht() {
    float h = dht.readHumidity();
    float t = dht.readTemperature();  // Celsius

    temperature = t;  // NaN is stored too, so the screen shows the failure
    humidity = h;

    bool failed = isnan(h) || isnan(t);
    recordScan(failed);

    if (failed) logLine("DHT22: read failed (NaN %d%% of last %d)", nanPercent(), historyCount);
    else logLine("DHT22: %.1f C, %.1f %%RH (NaN %d%% of last %d)", t, h, nanPercent(), historyCount);

    drawScreen();
}

// Returns true once per press (on the HIGH -> LOW edge), debounced
bool buttonPressed() {
    int reading = digitalRead(BUTTON_PIN);
    uint32_t now = millis();

    if (reading != lastButtonReading) {
        lastDebounceTime = now;
        lastButtonReading = reading;
    }

    if (now - lastDebounceTime >= DEBOUNCE_MS && reading != buttonState) {
        buttonState = reading;
        return buttonState == LOW;  // pressed pulls the pin to GND
    }
    return false;
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    pinMode(BUTTON_PIN, INPUT_PULLUP);  // idle HIGH, LOW when pressed

    dht.begin();

    canvas.setColorDepth(16);
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    drawScreen();
    printStatus();
}

void loop() {
    if (buttonPressed()) {
        quickMode = !quickMode;
        printStatus();
        drawScreen();
    }

    // Non-blocking blink, so button presses are detected immediately
    uint32_t interval = quickMode ? BLINK_INTERVAL_QUICK : BLINK_INTERVAL_SLOW;
    uint32_t now = millis();
    if (now - lastToggle >= interval) {
        lastToggle = now;
        ledOn = !ledOn;
        digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
        drawScreen();  // LED indicator follows the real LED
    }

    if (now - lastStatus >= STATUS_INTERVAL) {
        lastStatus = now;
        printStatus();
    }

    if (now - lastDhtRead >= DHT_INTERVAL) {
        lastDhtRead = now;
        readDht();
    }

    delay(5);
}