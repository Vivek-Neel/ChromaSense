// =============================================================================
// ChromaSense — main.cpp v4.9
// OTH Regensburg | Faculty ANK | Supervisor: Prof. Johannes Wild
// Author: Vivek Wilson Neelamkavil | 2026
//
// Platform : M5Stack Core2 v1.1 (ESP32 + AXP2101), M5Unified / Arduino
// Sensor   : AMS OSRAM TCS3530 XYZ tristimulus ALS
// Interface: I2C, SDA=GPIO32, SCL=GPIO33, 400 kHz (external Grove bus,
//            separate from the internal PMIC/touch bus on GPIO21/22)
// Driver   : ChromaSense_TCS3530 (24-bit FIFO, lib/ChromaSense_TCS3530)
//
// Outputs  : Illuminance (lux), CCT (K), CIE xy
// BT output: SPP Classic, "ChromaSense", ~10 Hz JSON
//   {"lux":N,"cct":N,"x":N.NNNN,"y":N.NNNN,
//    "X":N,"Y":N,"Z":N,"gain":"Nx","sat":N,"batt":N}
// =============================================================================

#include <M5Unified.h>
#include <BluetoothSerial.h>
#include <ChromaSense_TCS3530.h>
#include <math.h>

ChromaSense_TCS3530 sensor;
BluetoothSerial      SerialBT;
volatile bool        btConnected = false;

void btCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    if (event == ESP_SPP_SRV_OPEN_EVT) btConnected = true;
    if (event == ESP_SPP_CLOSE_EVT)    btConnected = false;
}

// ── RGB565 Colour Palette ────────────────────────────────────────────────────
#define C_BG        0x0841
#define C_PANEL     0x0863
#define C_HEADER    0x1083
#define C_BORDER    0x18E7
#define C_CYAN      0x073F
#define C_GREEN     0x07F1
#define C_RED_NEON  0xFA2C
#define C_AMBER     0xFD46
#define C_YELLOW    0xFF60
#define C_DIM       0x422C
#define C_MID       0x6331
#define C_WHITE     0xFFFF
#define C_BLACK     0x0000

#define CCT_CANDLE   0xFB00
#define CCT_WARM     0xFD46
#define CCT_NEUTRAL  0xFFBC
#define CCT_COOL     0xEFBF
#define CCT_DAYLIGHT 0xCF5F

// ── Battery Sprite ────────────────────────────────────────────────────────────
M5Canvas spriteBatt(&M5.Display);

// ── Software Auto-Gain ────────────────────────────────────────────────────────
// 3-level SW gain; sensor AGC disabled.

struct GainLevel {
    uint8_t     code;
    float       actual;
    const char* label;
};

static const GainLevel GAIN_LEVELS[] = {
    { TCS3530_GAIN_0_5X,  0.5f, "0.5x" },   // outdoor / direct sun
    { TCS3530_GAIN_2X,    2.0f, "2x"   },   // normal indoor (default)
    { TCS3530_GAIN_16X,  16.0f, "16x"  },   // dim / dark
};
static const int GAIN_LEVEL_COUNT = 3;

static const uint32_t PEAK_HIGH = 800000;
static const uint32_t PEAK_LOW  =   3000;

int  currentGainIdx  = 1;
bool gainJustChanged = false;

// =============================================================================
// AMS-OSRAM PER-SENSOR CALIBRATION (sensor 557D0C06B8)
// =============================================================================
// General calibration matrix A (3x4). Calibrated at ATIME=20ms, AGAIN=128x.
// Columns 5-8 are zero; only X,Y,Z,IR channels used (12-byte FIFO sufficient).
//
//   [X_cie]   [A11 A12 A13 A14]   [X_counts_norm]
//   [Y_cie] = [A21 A22 A23 A24] * [Y_counts_norm]
//   [Z_cie]   [A31 A32 A33 A34]   [Z_counts_norm]
//                                  [IR_counts_norm]
static const float CAL_A[3][4] = {
    {  0.013997f,  0.001168f, -0.000800f, -0.001339f },  // X_cie
    { -0.001871f,  0.016840f,  0.000258f, -0.000725f },  // Y_cie
    { -0.000893f,  0.001146f,  0.030002f, -0.001346f },  // Z_cie
};

static const float CAL_ATIME_MS = 20.0f;
static const float CAL_AGAIN    = 128.0f;
static const float OUR_ATIME_MS = 100.0f;   // (280+1)*(255+1)*1.3889us

// Per-modulator gain correction factors. Index: [0]=X [1]=Y [2]=Z [3]=IR.
// real_gain = nominal_gain * factor; normalises to CAL_AGAIN of 128x.
static const float GCORR_0_5X[4] = { 0.936f, 0.937f, 0.996f, 0.923f };
static const float GCORR_2X[4]   = { 1.000f, 1.008f, 0.996f, 1.000f };
static const float GCORR_16X[4]  = { 0.995f, 0.999f, 0.999f, 1.000f };
static const float* GCORR[]      = { GCORR_0_5X, GCORR_2X, GCORR_16X };

// Optical stack scalar: LUX_STACK_FACTOR = reference_lux / Y_cie.
static const float LUX_STACK_FACTOR = 1.0f;

// ── CCT Validity Range ────────────────────────────────────────────────────────
// McCamy (1992); values outside [1500, 20000] K → 0.
static const float CCT_MIN_VALID = 1500.0f;
static const float CCT_MAX_VALID = 20000.0f;

// ── Data Structure ────────────────────────────────────────────────────────────
struct ColorData {
    float    x, y;
    float    lux;
    float    cct;
    uint32_t rawX, rawY, rawZ;
    bool     saturated;
    int      gainIdx;
};

// ── Globals ───────────────────────────────────────────────────────────────────
bool needsRedraw = true;

// Dark mode: triggered by GUI command over BT SPP, or BtnB (3s hold) as a
// physical fallback. Backlight is controlled via M5.Display.setBrightness(),
// which M5GFX routes to the correct PMIC regulator for this board (AXP2101
// BLDO1 on Core2 v1.1).
bool darkMode = false;
static const int BT_CMD_BUF_SIZE = 32;
char btCmdBuf[BT_CMD_BUF_SIZE];
int  btCmdIdx = 0;

// ── Colour Science ────────────────────────────────────────────────────────────
uint16_t cctToColor(float cct) {
    if (cct <= 0)    return C_DIM;
    if (cct < 2200)  return CCT_CANDLE;
    if (cct < 3500)  return CCT_WARM;
    if (cct < 5000)  return CCT_NEUTRAL;
    if (cct < 6500)  return CCT_COOL;
    return CCT_DAYLIGHT;
}

// McCamy (1992): CCT = -449n^3 + 3525n^2 - 6823.3n + 5520.33
//   n = (x - 0.3320) / (y - 0.1858)
float calculateCCT(float x, float y) {
    if (fabsf(y - 0.1858f) < 1e-4f) return 0.0f;
    float n   = (x - 0.3320f) / (y - 0.1858f);
    float cct = -449.0f*n*n*n + 3525.0f*n*n - 6823.3f*n + 5520.33f;
    if (cct < CCT_MIN_VALID || cct > CCT_MAX_VALID) return 0.0f;
    return cct;
}

// ── Gain Control ──────────────────────────────────────────────────────────────
void applyGain(int gainIdx) {
    sensor.setGain(GAIN_LEVELS[gainIdx].code);
    sensor.writeRegister(TCS3530_REG_CONTROL, TCS3530_CONTROL_FIFO_CLR);
    Serial.printf("[GAIN] %s\n", GAIN_LEVELS[gainIdx].label);
}

bool checkAndUpdateGain(uint32_t peak, bool saturated) {
    int newIdx = currentGainIdx;
    if ((peak > PEAK_HIGH || saturated) && currentGainIdx > 0)
        newIdx = currentGainIdx - 1;
    else if (peak < PEAK_LOW && currentGainIdx < GAIN_LEVEL_COUNT - 1)
        newIdx = currentGainIdx + 1;

    if (newIdx != currentGainIdx) {
        currentGainIdx  = newIdx;
        gainJustChanged = true;
        applyGain(currentGainIdx);
        delay(120);   // one integration cycle at new gain before read
        return true;
    }
    return false;
}

// ── Core Processing ───────────────────────────────────────────────────────────
ColorData processSensorData(const TCS3530_data &d, int gainIdx) {
    ColorData c;
    c.saturated = d.saturated || !d.valid;
    c.gainIdx   = gainIdx;
    c.rawX = d.X; c.rawY = d.Y; c.rawZ = d.Z;

    if (c.saturated) {
        c.x = c.y = c.lux = c.cct = 0;
        return c;
    }

    // Normalise to cal reference: ATIME 100ms->20ms, gain per-channel->128x
    float atime_scale = CAL_ATIME_MS / OUR_ATIME_MS;

    const float* gcorr = GCORR[gainIdx];
    float nominal = GAIN_LEVELS[gainIdx].actual;
    float raw[4]  = { (float)d.X, (float)d.Y, (float)d.Z, (float)d.IR };
    float norm[4];
    for (int i = 0; i < 4; i++) {
        float real_gain = nominal * gcorr[i];
        norm[i] = raw[i] * atime_scale * (CAL_AGAIN / real_gain);
    }

    // Apply calibration matrix: [X,Y,Z]_cie = A * [X,Y,Z,IR]_norm
    float cie[3] = {0, 0, 0};
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 4; col++)
            cie[r] += CAL_A[r][col] * norm[col];

    float Xcie = cie[0] > 0 ? cie[0] : 0;
    float Ycie = cie[1] > 0 ? cie[1] : 0;
    float Zcie = cie[2] > 0 ? cie[2] : 0;

    c.lux = Ycie * LUX_STACK_FACTOR;

    float sum = Xcie + Ycie + Zcie;
    if (sum < 0.005f) {
        c.x = c.y = c.cct = 0;
        return c;
    }

    c.x   = Xcie / sum;
    c.y   = Ycie / sum;
    c.cct = calculateCCT(c.x, c.y);

    return c;
}

// ── Battery Median Filter (16-sample) ────────────────────────────────────────
#define BATT_SAMPLES 16
int  battSamples[BATT_SAMPLES] = {0};
int  battIdx  = 0;
bool battInit = false;

int battMedian() {
    int sorted[BATT_SAMPLES];
    for (int i = 0; i < BATT_SAMPLES; i++) sorted[i] = battSamples[i];
    for (int i = 1; i < BATT_SAMPLES; i++) {
        int key = sorted[i], j = i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j+1] = sorted[j]; j--; }
        sorted[j+1] = key;
    }
    return sorted[BATT_SAMPLES / 2];
}

int getBatteryPercent() {
    int raw = constrain((int)M5.Power.getBatteryLevel(), 0, 100);
    if (!battInit) {
        for (int i = 0; i < BATT_SAMPLES; i++) battSamples[i] = raw;
        battInit = true;
    }
    battSamples[battIdx] = raw;
    battIdx = (battIdx + 1) % BATT_SAMPLES;
    return battMedian();
}

// ── Battery Sprite ────────────────────────────────────────────────────────────
#define BATT_SPR_W 136
#define BATT_SPR_H  30
#define BATT_SCR_X 170
#define BATT_SCR_Y 184

void initSprites() {
    spriteBatt.createSprite(BATT_SPR_W, BATT_SPR_H);
}

void drawBattSprite(int battPct) {
    spriteBatt.fillSprite(C_PANEL);
    uint16_t battCol = C_GREEN;
    if      (battPct < 20) battCol = 0xF800;
    else if (battPct < 50) battCol = C_AMBER;
    int bFill = constrain((int)((battPct / 100.0f) * BATT_SPR_W), 0, BATT_SPR_W);
    spriteBatt.fillRect(0, 0, BATT_SPR_W, BATT_SPR_H, C_BORDER);
    if (bFill > 0) spriteBatt.fillRect(0, 0, bFill, BATT_SPR_H, battCol);
    char battStr[6];
    sprintf(battStr, "%d%%", battPct);
    int tx = (BATT_SPR_W / 2) - (strlen(battStr) * 6);
    spriteBatt.setTextFont(2);
    spriteBatt.setTextColor(C_BLACK, battCol);
    spriteBatt.setCursor(tx, 9);
    spriteBatt.print(battStr);
    spriteBatt.pushSprite(BATT_SCR_X, BATT_SCR_Y);
}

// ── Static UI Layout ──────────────────────────────────────────────────────────
void drawStaticUI() {
    M5.Display.fillScreen(C_BG);

    M5.Display.fillRect(0, 0, 320, 32, C_HEADER);
    M5.Display.drawFastHLine(0, 32, 320, C_CYAN);
    M5.Display.setTextFont(2);
    M5.Display.setTextColor(C_CYAN, C_HEADER);
    M5.Display.setCursor(10, 10);
    M5.Display.print("CHROMASENSE");

    M5.Display.fillRect(6, 38, 148, 58, C_PANEL);
    M5.Display.drawRect(6, 38, 148, 58, C_BORDER);
    M5.Display.setTextFont(1); M5.Display.setTextColor(C_DIM, C_PANEL);
    M5.Display.setCursor(12, 44); M5.Display.print("ILLUMINANCE");

    M5.Display.fillRect(6, 100, 148, 58, C_PANEL);
    M5.Display.drawRect(6, 100, 148, 58, C_BORDER);
    M5.Display.setTextFont(1); M5.Display.setTextColor(C_DIM, C_PANEL);
    M5.Display.setCursor(12, 106); M5.Display.print("COLOR TEMP");

    M5.Display.fillRect(6, 162, 148, 70, C_PANEL);
    M5.Display.drawRect(6, 162, 148, 70, C_BORDER);
    M5.Display.setTextFont(1); M5.Display.setTextColor(C_DIM, C_PANEL);
    M5.Display.setCursor(12, 168); M5.Display.print("CIE CHROMATICITY");
    M5.Display.setTextFont(2); M5.Display.setTextColor(C_MID, C_PANEL);
    M5.Display.setCursor(12, 184); M5.Display.print("x");
    M5.Display.setCursor(12, 208); M5.Display.print("y");

    M5.Display.fillRect(162, 38, 152, 120, C_PANEL);
    M5.Display.drawRect(162, 38, 152, 120, C_BORDER);
    M5.Display.setTextFont(1); M5.Display.setTextColor(C_DIM, C_PANEL);
    M5.Display.setCursor(168, 44); M5.Display.print("RAW COUNTS (24-bit)");
    M5.Display.setTextFont(2);
    M5.Display.setTextColor(C_RED_NEON, C_PANEL); M5.Display.setCursor(168, 60);  M5.Display.print("X");
    M5.Display.setTextColor(C_GREEN,    C_PANEL); M5.Display.setCursor(168, 86);  M5.Display.print("Y");
    M5.Display.setTextColor(C_CYAN,     C_PANEL); M5.Display.setCursor(168, 112); M5.Display.print("Z");

    M5.Display.fillRect(162, 162, 152, 70, C_PANEL);
    M5.Display.drawRect(162, 162, 152, 70, C_BORDER);
    M5.Display.setTextFont(1); M5.Display.setTextColor(C_DIM, C_PANEL);
    M5.Display.setCursor(168, 168); M5.Display.print("BATTERY");

    M5.Display.drawFastHLine(0, 238, 320, C_CYAN);
}

// ── Dynamic UI Refresh ────────────────────────────────────────────────────────
void drawDynamicUI(ColorData &d, int battPct) {
    uint16_t btCol = btConnected ? C_GREEN : 0xF800;
    M5.Display.fillCircle(274, 16, 5, btCol);
    M5.Display.setTextFont(1); M5.Display.setTextColor(btCol, C_HEADER);
    M5.Display.setCursor(282, 12);
    M5.Display.print(btConnected ? "CONN" : "WAIT");

    M5.Display.setTextFont(1); M5.Display.setTextColor(C_DIM, C_HEADER);
    M5.Display.fillRect(195, 2, 60, 18, C_HEADER);
    M5.Display.setCursor(200, 12);
    M5.Display.printf("G:%s", GAIN_LEVELS[d.gainIdx].label);

    uint16_t luxCol = d.saturated ? 0xF800 : C_WHITE;
    M5.Display.setTextFont(4); M5.Display.setTextColor(luxCol, C_PANEL);
    M5.Display.setCursor(12, 54); M5.Display.printf("%-6.0f", d.lux);
    M5.Display.setTextFont(2); M5.Display.setTextColor(C_MID, C_PANEL);
    M5.Display.setCursor(118, 76); M5.Display.print("lx");

    uint16_t cctCol = d.saturated ? 0xF800 : cctToColor(d.cct);
    M5.Display.setTextFont(4); M5.Display.setTextColor(cctCol, C_PANEL);
    M5.Display.setCursor(12, 116);
    if (d.cct > 0) M5.Display.printf("%-6.0f", d.cct);
    else           M5.Display.print("--    ");
    M5.Display.setTextFont(2); M5.Display.setTextColor(C_MID, C_PANEL);
    M5.Display.setCursor(118, 138); M5.Display.print("K");

    M5.Display.setTextFont(2);
    M5.Display.setTextColor(C_RED_NEON, C_PANEL);
    M5.Display.setCursor(184, 60);  M5.Display.printf("%-7lu", (unsigned long)d.rawX);
    M5.Display.setTextColor(C_GREEN, C_PANEL);
    M5.Display.setCursor(184, 86);  M5.Display.printf("%-7lu", (unsigned long)d.rawY);
    M5.Display.setTextColor(C_CYAN,  C_PANEL);
    M5.Display.setCursor(184, 112); M5.Display.printf("%-7lu", (unsigned long)d.rawZ);

    M5.Display.setTextFont(2); M5.Display.setTextColor(C_YELLOW, C_PANEL);
    M5.Display.setCursor(26, 184); M5.Display.printf("%.4f", d.x);
    M5.Display.setCursor(26, 208); M5.Display.printf("%.4f", d.y);

    drawBattSprite(battPct);
}

// ── Dark mode command handler ───────────────────────────────────────────────
void enterDarkMode() {
    M5.Display.setBrightness(0);   // board-aware: AXP2101 BLDO1 on this hardware
    M5.Power.setLed(0);
    darkMode = true;
    Serial.println("[DARK] entered");
}

void exitDarkMode() {
    M5.Display.setBrightness(200);
    M5.Power.setLed(255);
    darkMode = false;
    needsRedraw = true;
    Serial.println("[DARK] exited");
}

void checkBtCommands() {
    while (SerialBT.available()) {
        char c = SerialBT.read();
        if (c == '\n' || c == '\r') {
            if (btCmdIdx == 0) continue;   // skip blank line from \r\n pair
            btCmdBuf[btCmdIdx] = '\0';
            Serial.printf("[BT-CMD] received: \"%s\"\n", btCmdBuf);
            if (strcmp(btCmdBuf, "DARK") == 0 && !darkMode)  enterDarkMode();
            if (strcmp(btCmdBuf, "LIGHT") == 0 && darkMode)  exitDarkMode();
            btCmdIdx = 0;
        } else if (btCmdIdx < BT_CMD_BUF_SIZE - 1) {
            btCmdBuf[btCmdIdx++] = c;
        }
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Display.setBrightness(200);
    M5.Display.fillScreen(C_BG);

    M5.Display.setTextFont(2); M5.Display.setTextColor(C_CYAN, C_BG);
    M5.Display.setCursor(10, 10); M5.Display.println("CHROMASENSE");
    M5.Display.setTextColor(C_GREEN, C_BG);
    M5.Display.setCursor(10, 35); M5.Display.println("Initializing TCS3530...");

    bool sensorReady = sensor.begin(32, 33, 400000, 280,
                                     GAIN_LEVELS[currentGainIdx].code);

    M5.Display.setTextColor(sensorReady ? C_GREEN : 0xF800, C_BG);
    M5.Display.setCursor(10, 60);
    M5.Display.println(sensorReady ? "TCS3530 OK" : "TCS3530 NOT FOUND");

    M5.Display.setTextColor(C_GREEN, C_BG);
    M5.Display.setCursor(10, 85); M5.Display.println("Starting Bluetooth...");
    SerialBT.begin("ChromaSense");
    SerialBT.register_callback(btCallback);

    M5.Display.setTextColor(C_WHITE, C_BG);
    M5.Display.setCursor(10, 110); M5.Display.println("BT Ready!");

    Serial.println("ChromaSense v4.9 ready (M5Unified / Core2 v1.1).");
    delay(1500);
    initSprites();
    drawStaticUI();
    needsRedraw = false;
}

// ── Main Loop ─────────────────────────────────────────────────────────────────
void loop() {
    M5.update();
    checkBtCommands();

    // BtnB is a physical fallback: hold for 3s to toggle dark mode,
    // independent of BT. holdTriggered prevents repeat firing while held
    // and resets on release.
    static bool holdTriggered = false;
    if (M5.BtnB.isPressed()) {
        if (!holdTriggered && M5.BtnB.pressedFor(3000)) {
            holdTriggered = true;
            if (darkMode) exitDarkMode();
            else          enterDarkMode();
        }
    } else {
        holdTriggered = false;
    }

    TCS3530_data raw = sensor.read();
    if (!raw.valid) { delay(200); return; }

    uint32_t peak = raw.X;
    if (raw.Y > peak) peak = raw.Y;
    if (raw.Z > peak) peak = raw.Z;

    if (checkAndUpdateGain(peak, raw.saturated)) {
        if (!darkMode) needsRedraw = true;
        return;
    }

    ColorData d    = processSensorData(raw, currentGainIdx);
    int       batt = getBatteryPercent();

    char json[240];
    snprintf(json, sizeof(json),
        "{\"lux\":%.0f,\"cct\":%.0f,\"x\":%.4f,\"y\":%.4f,"
        "\"X\":%lu,\"Y\":%lu,\"Z\":%lu,"
        "\"gain\":\"%s\",\"sat\":%d,\"batt\":%d,\"dark\":%d}\n",
        d.lux, d.cct, d.x, d.y,
        (unsigned long)d.rawX, (unsigned long)d.rawY, (unsigned long)d.rawZ,
        GAIN_LEVELS[d.gainIdx].label, (int)d.saturated, batt, (int)darkMode);

    Serial.print(json);
    if (btConnected) SerialBT.print(json);

    if (darkMode) { delay(100); return; }

    if (needsRedraw) {
        drawStaticUI();
        needsRedraw = false;
    }

    drawDynamicUI(d, batt);
    delay(100);
}