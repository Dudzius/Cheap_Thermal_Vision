// ============================================================================
//  CHEAP THERMAL VISION  V2.0
//  ESP32-2432S028 ("Cheap Yellow Display", 240x320 ILI9341) + MLX90640
//
//  - Landscape 320x240 layout, correct 4:3 image (32x24 sensor x8 = 256x192)
//  - Center-point temperature (big), min / max, color scale, battery indicator
//  - Smart auto-range: ignores outliers, enforces a minimum span, and moves
//    slowly so colors stay calm instead of flashing
//  - Touch anywhere: freeze the picture. Touch again: back to live
//  - Sensor is read in its own task, so touch stays responsive
// ============================================================================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <Adafruit_MLX90640.h>

// ============================================================================
//  SETTINGS  (the things you will most likely want to tweak)
// ============================================================================

// --- Orientation -------------------------------------------------------------
#define SCREEN_ROTATION 1      // 1 = landscape. Use 3 for landscape upside-down.
#define FLIP_X false           // mirror the thermal image left <-> right
#define FLIP_Y false           // mirror the thermal image top <-> bottom

// --- Sensor ------------------------------------------------------------------
#define SENSOR_REFRESH MLX90640_8_HZ   // try MLX90640_4_HZ if you get read errors
#define I2C_CLOCK 800000               // try 400000 if you get read errors

// --- Auto range (the "not hallucinogenic" part) ------------------------------
#define USE_FIXED_RANGE false  // true = colors always cover FIXED_RANGE_LO..HI
#define FIXED_RANGE_LO 15.0f
#define FIXED_RANGE_HI 40.0f

#define RANGE_MIN_SPAN 8.0f    // color scale is never narrower than this (deg C).
                               // Lower (5-6) = more sensitive to tiny differences
                               // (draft hunting), higher = calmer.
#define RANGE_CLIP 0.005f      // ignore this fraction of the hottest and coldest
                               // pixels when picking the range (noise / tiny spikes)
#define RANGE_GROW 0.35f       // how fast the scale WIDENS (hot thing enters view)
#define RANGE_SHRINK 0.05f     // how fast the scale NARROWS (slow = calm)
#define RANGE_DEADBAND 0.5f    // ignore range changes smaller than this (deg C)

// --- Battery indicator -------------------------------------------------------
// The CYD cannot measure the battery on its own. To enable it, wire a divider
// from the 18650 positive terminal:
//
//    battery (+) ---[100k]---+---[100k]--- GND
//                            |
//                         GPIO 35
//
// Then set BATTERY_ENABLED to 1.
#define BATTERY_ENABLED 0
#define BATT_PIN 35
#define BATT_DIVIDER 2.0f      // (R1 + R2) / R2  -> 2.0 for two equal resistors
#define BATT_CAL 1.00f         // fine tune: real voltage / displayed voltage

// ============================================================================
//  PINS
// ============================================================================

#define I2C_SDA 27
#define I2C_SCL 22
#define TOUCH_IRQ 36           // XPT2046 pen interrupt: LOW while screen is touched

// ============================================================================
//  GEOMETRY
// ============================================================================

#define SCREEN_W 320
#define SCREEN_H 240

#define GRID_W 32
#define GRID_H 24
#define PIXEL_COUNT (GRID_W * GRID_H)

#define SCALE 8
#define IMG_W (GRID_W * SCALE)   // 256
#define IMG_H (GRID_H * SCALE)   // 192
#define IMG_X 0
#define IMG_Y 16

#define CROSS_X (IMG_W / 2)
#define CROSS_Y (IMG_H / 2)

#define BAR_X 266
#define BAR_W 16
#define BOTTOM_Y 208

// ============================================================================
//  DATA
// ============================================================================

struct ThermalFrame {
  float pixels[PIXEL_COUNT];
  float minTemp;
  float maxTemp;
  float center;
};

TFT_eSPI tft = TFT_eSPI();
Adafruit_MLX90640 mlx;
ThermalFrame frame;

// Known dead pixels of YOUR sensor (NaN pixels are also fixed automatically)
static const uint16_t KNOWN_BAD_PIXELS[] = {258, 601};
static bool knownBad[PIXEL_COUNT];

// Sensor task -> main loop hand-over
static float sharedPixels[PIXEL_COUNT];
static volatile bool frameReady = false;
static volatile bool sensorOk = true;
static SemaphoreHandle_t frameMutex;

// Interpolation lookup tables
static uint8_t xIdx0[IMG_W], xIdx1[IMG_W];
static float xFrac[IMG_W];
static uint8_t yIdx0[IMG_H], yIdx1[IMG_H];
static float yFrac[IMG_H];

// Palette lookup (256 steps: blue - cyan - green - yellow - orange - red)
static uint16_t paletteLut[256];

// Color range state
static float rangeLo = 20.0f;
static float rangeHi = 30.0f;
static bool rangeInit = false;

// UI state
static bool frozen = false;
static int lastStatus = -1;
static int lastCenterTenths = -9999;
static int lastMinTenths = -9999;
static int lastMaxTenths = -9999;
static int lastLabelHi = -9999, lastLabelMid = -9999, lastLabelLo = -9999;
static float battVoltage = 0.0f;
static int battPercent = -1;
static uint32_t lastBattMs = 0;

// ============================================================================
//  PALETTE
// ============================================================================

void buildPalette() {
  for (int i = 0; i < 256; i++) {
    float value = i / 255.0f;
    uint8_t r, g, b;

    if (value < 0.20f) {                    // BLUE -> CYAN
      float t = value / 0.20f;
      r = 0;
      g = (uint8_t)(255.0f * t);
      b = 255;
    } else if (value < 0.40f) {             // CYAN -> GREEN
      float t = (value - 0.20f) / 0.20f;
      r = 0;
      g = 255;
      b = (uint8_t)(255.0f * (1.0f - t));
    } else if (value < 0.60f) {             // GREEN -> YELLOW
      float t = (value - 0.40f) / 0.20f;
      r = (uint8_t)(255.0f * t);
      g = 255;
      b = 0;
    } else if (value < 0.80f) {             // YELLOW -> ORANGE
      float t = (value - 0.60f) / 0.20f;
      r = 255;
      g = (uint8_t)(255.0f - (90.0f * t));
      b = 0;
    } else {                                // ORANGE -> RED
      float t = (value - 0.80f) / 0.20f;
      r = 255;
      g = (uint8_t)(165.0f * (1.0f - t));
      b = 0;
    }
    paletteLut[i] = tft.color565(r, g, b);
  }
}

// ============================================================================
//  INTERPOLATION TABLES
// ============================================================================

void buildInterpolationTables() {
  for (int x = 0; x < IMG_W; x++) {
    float sx = (x + 0.5f) / SCALE - 0.5f;
    sx = constrain(sx, 0.0f, (float)(GRID_W - 1));
    if (FLIP_X) sx = (GRID_W - 1) - sx;
    int x0 = (int)sx;
    xIdx0[x] = x0;
    xIdx1[x] = min(x0 + 1, GRID_W - 1);
    xFrac[x] = sx - x0;
  }
  for (int y = 0; y < IMG_H; y++) {
    float sy = (y + 0.5f) / SCALE - 0.5f;
    sy = constrain(sy, 0.0f, (float)(GRID_H - 1));
    if (FLIP_Y) sy = (GRID_H - 1) - sy;
    int y0 = (int)sy;
    yIdx0[y] = y0;
    yIdx1[y] = min(y0 + 1, GRID_H - 1);
    yFrac[y] = sy - y0;
  }
}

// ============================================================================
//  HARDWARE INIT
// ============================================================================

void initDisplay() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.setSwapBytes(true);           // needed for pushImage with RGB565 buffers
  tft.fillScreen(TFT_BLACK);
}

void initI2C() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_CLOCK);
}

bool initThermalSensor() {
  Serial.println("Initializing MLX90640...");

  if (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
    Serial.println("MLX90640 initialization FAILED");
    return false;
  }

  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(SENSOR_REFRESH);
  return true;
}

// ============================================================================
//  STARTUP SCREEN
// ============================================================================

void showStartupScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("CHEAP", SCREEN_W / 2, 35, 4);
  tft.drawString("THERMAL", SCREEN_W / 2, 67, 4);
  tft.drawString("VISION", SCREEN_W / 2, 99, 4);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("DISPLAY       OK", SCREEN_W / 2, 150, 2);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("MLX90640      ...", SCREEN_W / 2, 175, 2);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("V2.0", SCREEN_W / 2, 215, 2);
}

void showSensorStatus(bool success) {
  tft.fillRect(60, 163, 200, 24, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  if (success) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("MLX90640      OK", SCREEN_W / 2, 175, 2);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("MLX90640      ERROR", SCREEN_W / 2, 175, 2);
  }
}

// ============================================================================
//  SENSOR TASK  (runs on core 0, owns the I2C bus after setup)
// ============================================================================

void sensorTask(void* param) {
  static float tmp[PIXEL_COUNT];
  int failCount = 0;

  for (;;) {
    int status = mlx.getFrame(tmp);

    if (status < 0) {
      Serial.print("MLX90640 frame read failed, status: ");
      Serial.println(status);
      if (++failCount >= 5) sensorOk = false;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    failCount = 0;
    sensorOk = true;

    xSemaphoreTake(frameMutex, portMAX_DELAY);
    memcpy(sharedPixels, tmp, sizeof(tmp));
    frameReady = true;
    xSemaphoreGive(frameMutex);

    vTaskDelay(1);   // let the idle task breathe
  }
}

bool takeNewFrame() {
  if (!frameReady) return false;

  xSemaphoreTake(frameMutex, portMAX_DELAY);
  memcpy(frame.pixels, sharedPixels, sizeof(frame.pixels));
  frameReady = false;
  xSemaphoreGive(frameMutex);
  return true;
}

// ============================================================================
//  FRAME PROCESSING
// ============================================================================

// Average of the valid (non-bad) direct neighbors of a pixel
static bool averageValidNeighbors(int index, const bool* bad, float* result) {
  int row = index / GRID_W;
  int col = index % GRID_W;
  float sum = 0.0f;
  int count = 0;

  if (col > 0 && !bad[index - 1]) { sum += frame.pixels[index - 1]; count++; }
  if (col < GRID_W - 1 && !bad[index + 1]) { sum += frame.pixels[index + 1]; count++; }
  if (row > 0 && !bad[index - GRID_W]) { sum += frame.pixels[index - GRID_W]; count++; }
  if (row < GRID_H - 1 && !bad[index + GRID_W]) { sum += frame.pixels[index + GRID_W]; count++; }

  if (count == 0) return false;
  *result = sum / count;
  return true;
}

// Replace dead / NaN pixels, then compute min, max and center temperature
void cleanFrame() {
  static bool bad[PIXEL_COUNT];

  float sum = 0.0f;
  int valid = 0;

  for (int i = 0; i < PIXEL_COUNT; i++) {
    bad[i] = knownBad[i] || isnan(frame.pixels[i]) || isinf(frame.pixels[i]);
    if (!bad[i]) { sum += frame.pixels[i]; valid++; }
  }
  float mean = valid > 0 ? sum / valid : 25.0f;

  for (int i = 0; i < PIXEL_COUNT; i++) {
    if (!bad[i]) continue;
    float avg;
    frame.pixels[i] = averageValidNeighbors(i, bad, &avg) ? avg : mean;
  }

  frame.minTemp = frame.pixels[0];
  frame.maxTemp = frame.pixels[0];
  for (int i = 1; i < PIXEL_COUNT; i++) {
    if (frame.pixels[i] < frame.minTemp) frame.minTemp = frame.pixels[i];
    if (frame.pixels[i] > frame.maxTemp) frame.maxTemp = frame.pixels[i];
  }

  // Center = average of the 4 middle pixels (the exact middle of a 32x24 grid)
  int r0 = GRID_H / 2 - 1;
  int c0 = GRID_W / 2 - 1;
  frame.center = (frame.pixels[r0 * GRID_W + c0] + frame.pixels[r0 * GRID_W + c0 + 1] +
                  frame.pixels[(r0 + 1) * GRID_W + c0] + frame.pixels[(r0 + 1) * GRID_W + c0 + 1]) / 4.0f;
}

static int cmpFloat(const void* a, const void* b) {
  float x = *(const float*)a;
  float y = *(const float*)b;
  return (x > y) - (x < y);
}

static float followTarget(float current, float target, bool widening) {
  float diff = target - current;
  if (fabsf(diff) < RANGE_DEADBAND) return current;
  return current + diff * (widening ? RANGE_GROW : RANGE_SHRINK);
}

//  Auto range:
//   1. Look at the scene without its hottest / coldest few pixels (outliers).
//   2. Never let the scale get narrower than RANGE_MIN_SPAN, so a flat scene is
//      not stretched into a rainbow of pure noise.
//   3. Move the scale slowly (fast to widen, slow to narrow) with a dead band,
//      so colors do not pulse or flash.
void updateRange() {
  if (USE_FIXED_RANGE) {
    rangeLo = FIXED_RANGE_LO;
    rangeHi = FIXED_RANGE_HI;
    return;
  }

  static float sorted[PIXEL_COUNT];
  memcpy(sorted, frame.pixels, sizeof(sorted));
  qsort(sorted, PIXEL_COUNT, sizeof(float), cmpFloat);

  int clipCount = (int)(PIXEL_COUNT * RANGE_CLIP);
  float lo = sorted[clipCount];
  float hi = sorted[PIXEL_COUNT - 1 - clipCount];

  if (hi - lo < RANGE_MIN_SPAN) {
    float mid = (lo + hi) * 0.5f;
    lo = mid - RANGE_MIN_SPAN * 0.5f;
    hi = mid + RANGE_MIN_SPAN * 0.5f;
  }

  if (!rangeInit) {
    rangeLo = lo;
    rangeHi = hi;
    rangeInit = true;
    return;
  }

  rangeHi = followTarget(rangeHi, hi, hi > rangeHi);
  rangeLo = followTarget(rangeLo, lo, lo < rangeLo);
}

// ============================================================================
//  RENDERING
// ============================================================================

// Small white "+" with a black outline, drawn straight into the image buffer
// (so it never flickers)
static void overlayCrosshair(uint16_t* band, int bandY) {
  for (int r = 0; r < SCALE; r++) {
    int dy = bandY + r - CROSS_Y;
    if (dy < -9 || dy > 9) continue;

    uint16_t* row = band + r * IMG_W;
    for (int dx = -9; dx <= 9; dx++) {
      int adx = abs(dx);
      int ady = abs(dy);
      bool horizArm = (adx >= 3 && adx <= 8);
      bool vertArm = (ady >= 3 && ady <= 8);

      bool white = (horizArm && dy == 0) || (vertArm && dx == 0);
      bool shadow = (horizArm && ady == 1) || (vertArm && adx == 1);

      if (white) row[CROSS_X + dx] = 0xFFFF;
      else if (shadow) row[CROSS_X + dx] = 0x0000;
    }
  }
}

// Interpolates 32x24 -> 256x192, colors it and pushes it in 8-line strips
void renderThermalImage() {
  static uint16_t band[IMG_W * SCALE];

  float lo = rangeLo;
  float scaleFactor = 255.0f / (rangeHi - rangeLo);

  for (int by = 0; by < IMG_H; by += SCALE) {
    for (int r = 0; r < SCALE; r++) {
      int y = by + r;
      const float* row0 = frame.pixels + yIdx0[y] * GRID_W;
      const float* row1 = frame.pixels + yIdx1[y] * GRID_W;
      float fy = yFrac[y];
      uint16_t* out = band + r * IMG_W;

      for (int x = 0; x < IMG_W; x++) {
        int x0 = xIdx0[x];
        int x1 = xIdx1[x];
        float fx = xFrac[x];

        float top = row0[x0] + (row0[x1] - row0[x0]) * fx;
        float bottom = row1[x0] + (row1[x1] - row1[x0]) * fx;
        float temperature = top + (bottom - top) * fy;

        int idx = (int)((temperature - lo) * scaleFactor);
        if (idx < 0) idx = 0;
        if (idx > 255) idx = 255;
        out[x] = paletteLut[idx];
      }
    }

    overlayCrosshair(band, by);
    tft.pushImage(IMG_X, IMG_Y + by, IMG_W, SCALE, band);
  }
}

// Temperature with a drawn degree sign, e.g. "36.5 C" (font 2 or 4)
static void drawTemp(int x, int y, float value, uint8_t font, uint16_t color, int clearW) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%.1f", value);

  tft.fillRect(x, y, clearW, tft.fontHeight(font), TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, TFT_BLACK);

  int w = tft.drawString(buf, x, y, font);
  int rad = (font == 4) ? 3 : 2;
  tft.drawCircle(x + w + rad + 2, y + rad + 3, rad, color);
  tft.drawString("C", x + w + 2 * rad + 5, y, font);
}

static void drawScaleLabel(int y, int value) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  tft.fillRect(BAR_X + BAR_W + 3, y, SCREEN_W - (BAR_X + BAR_W + 3), 16, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(buf, BAR_X + BAR_W + 5, y, 2);
}

// Parts of the screen that never change
void drawStaticUI() {
  tft.fillScreen(TFT_BLACK);

  // Color scale bar (hot at the top)
  for (int y = 0; y < IMG_H; y++) {
    int idx = 255 - (y * 255) / (IMG_H - 1);
    tft.drawFastHLine(BAR_X, IMG_Y + y, BAR_W, paletteLut[idx]);
  }
  tft.drawRect(BAR_X - 1, IMG_Y - 1, BAR_W + 2, IMG_H + 2, TFT_DARKGREY);

  // Labels in the bottom bar
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("MAX", 190, BOTTOM_Y, 2);
  tft.drawString("MIN", 190, BOTTOM_Y + 16, 2);

  // Force everything dynamic to redraw
  lastStatus = -1;
  lastCenterTenths = lastMinTenths = lastMaxTenths = -9999;
  lastLabelHi = lastLabelMid = lastLabelLo = -9999;
  battPercent = -1;
}

// Numbers that change with the picture
void updateReadouts() {
  int ct = lroundf(frame.center * 10.0f);
  if (ct != lastCenterTenths) {
    lastCenterTenths = ct;
    drawTemp(8, BOTTOM_Y + 3, ct / 10.0f, 4, TFT_WHITE, 150);
  }

  int mx = lroundf(frame.maxTemp * 10.0f);
  if (mx != lastMaxTenths) {
    lastMaxTenths = mx;
    drawTemp(228, BOTTOM_Y, mx / 10.0f, 2, TFT_ORANGE, 90);
  }

  int mn = lroundf(frame.minTemp * 10.0f);
  if (mn != lastMinTenths) {
    lastMinTenths = mn;
    drawTemp(228, BOTTOM_Y + 16, mn / 10.0f, 2, TFT_CYAN, 90);
  }

  // Scale labels (whole degrees)
  int hi = lroundf(rangeHi);
  int mid = lroundf((rangeHi + rangeLo) * 0.5f);
  int lo = lroundf(rangeLo);
  if (hi != lastLabelHi) { lastLabelHi = hi; drawScaleLabel(IMG_Y, hi); }
  if (mid != lastLabelMid) { lastLabelMid = mid; drawScaleLabel(IMG_Y + IMG_H / 2 - 8, mid); }
  if (lo != lastLabelLo) { lastLabelLo = lo; drawScaleLabel(IMG_Y + IMG_H - 16, lo); }
}

// LIVE / FROZEN / SENSOR ERROR in the top-left corner
void updateStatus() {
  int status = !sensorOk ? 2 : (frozen ? 1 : 0);
  if (status == lastStatus) return;
  lastStatus = status;

  tft.fillRect(0, 0, 200, 16, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  if (status == 0) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("LIVE", 4, 0, 2);
  } else if (status == 1) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("FROZEN - tap to resume", 4, 0, 2);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("SENSOR ERROR", 4, 0, 2);
  }
}

// ============================================================================
//  BATTERY
// ============================================================================

// Typical resting voltage curve of a Li-ion 18650
static int batteryPercentFromVoltage(float v) {
  static const float volts[] = {3.27f, 3.61f, 3.69f, 3.71f, 3.73f, 3.75f, 3.77f, 3.79f, 3.80f,
                                3.82f, 3.84f, 3.85f, 3.87f, 3.91f, 3.95f, 3.98f, 4.02f, 4.08f,
                                4.11f, 4.15f, 4.20f};
  static const int pct[] = {0, 5, 10, 15, 20, 25, 30, 35, 40,
                            45, 50, 55, 60, 65, 70, 75, 80, 85,
                            90, 95, 100};
  const int n = sizeof(volts) / sizeof(volts[0]);

  if (v <= volts[0]) return 0;
  if (v >= volts[n - 1]) return 100;

  for (int i = 1; i < n; i++) {
    if (v <= volts[i]) {
      float t = (v - volts[i - 1]) / (volts[i] - volts[i - 1]);
      return (int)lroundf(pct[i - 1] + t * (pct[i] - pct[i - 1]));
    }
  }
  return 100;
}

void drawBattery(int percent) {
  tft.fillRect(220, 0, 100, 16, TFT_BLACK);

  uint16_t color = (percent > 30) ? TFT_GREEN : (percent > 15 ? TFT_YELLOW : TFT_RED);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", percent);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(buf, 286, 0, 2);

  // Icon: body 26x11 + nub
  tft.drawRect(290, 3, 26, 11, color);
  tft.fillRect(316, 6, 2, 5, color);
  int fillW = (percent * 22) / 100;
  if (fillW > 0) tft.fillRect(292, 5, fillW, 7, color);
}

void updateBattery() {
#if BATTERY_ENABLED
  uint32_t now = millis();
  if (battPercent >= 0 && now - lastBattMs < 2000) return;
  lastBattMs = now;

  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BATT_PIN);
  float v = (sum / 16.0f) / 1000.0f * BATT_DIVIDER * BATT_CAL;

  battVoltage = (battVoltage < 1.0f) ? v : battVoltage * 0.8f + v * 0.2f;
  int p = batteryPercentFromVoltage(battVoltage);

  if (p != battPercent) {
    battPercent = p;
    drawBattery(p);
  }
#endif
}

// ============================================================================
//  TOUCH  (tap anywhere = freeze / resume)
// ============================================================================

void handleTouch() {
  static uint32_t downSince = 0;
  static bool handled = false;
  static uint32_t lastToggle = 0;

  bool down = (digitalRead(TOUCH_IRQ) == LOW);

  if (down) {
    if (downSince == 0) {
      downSince = millis();
    } else if (!handled && millis() - downSince >= 30 && millis() - lastToggle > 300) {
      frozen = !frozen;
      handled = true;
      lastToggle = millis();
      updateStatus();
    }
  } else {
    downSince = 0;
    handled = false;
  }
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("      CHEAP THERMAL VISION V2");

  pinMode(TOUCH_IRQ, INPUT);

#if BATTERY_ENABLED
  analogSetPinAttenuation(BATT_PIN, ADC_11db);
#endif

  for (size_t i = 0; i < sizeof(KNOWN_BAD_PIXELS) / sizeof(KNOWN_BAD_PIXELS[0]); i++) {
    knownBad[KNOWN_BAD_PIXELS[i]] = true;
  }

  initI2C();
  initDisplay();
  buildPalette();
  buildInterpolationTables();
  showStartupScreen();

  // Keep trying until the sensor answers
  while (!initThermalSensor()) {
    showSensorStatus(false);
    delay(1500);
  }
  showSensorStatus(true);
  delay(1200);

  drawStaticUI();

  frameMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(sensorTask, "sensor", 8192, NULL, 1, NULL, 0);
}

void loop() {
  handleTouch();

  if (!frozen && takeNewFrame()) {
    cleanFrame();
    updateRange();
    renderThermalImage();
    updateReadouts();
  }

  updateStatus();
  updateBattery();

  delay(2);
}
