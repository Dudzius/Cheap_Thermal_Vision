#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <Adafruit_MLX90640.h>

#define I2C_SDA 27
#define I2C_SCL 22

#define SCREEN_W 270
#define SCREEN_H 340

#define GRID_W 32
#define GRID_H 24
#define PIXEL_COUNT (GRID_W * GRID_H)

#define RENDER_W 90
#define RENDER_H 68

struct ThermalFrame {
  float pixels[PIXEL_COUNT];

  float minTemp;
  float maxTemp;
};

float renderFrame[RENDER_W * RENDER_H];

//                                      Objects

TFT_eSPI tft = TFT_eSPI();
Adafruit_MLX90640 mlx;
ThermalFrame frame;

//                                          Display initialization

void initDisplay() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();

  tft.setRotation(0);

  tft.fillScreen(TFT_BLACK);
}


//                                  I2C initialization

void initI2C() {
  Wire.begin(I2C_SDA, I2C_SCL);

  Wire.setClock(400000);
}

//                                  MLX90640 initialization

bool initThermalSensor() {
  Serial.println("Initializing MLX90640...");

  // The default MLX90640 address is 0x33.
  if (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
    Serial.println("MLX90640 initialization FAILED");

    return false;
  }

  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(MLX90640_4_HZ);

  return true;
}

//                                  Startup screen

void showStartupScreen() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);

  tft.setTextDatum(MC_DATUM);

  tft.drawString(
    "CHEAP",
    SCREEN_W
   / 2,
    45,
    4);

  tft.drawString(
    "THERMAL",
    SCREEN_W
   / 2,
    80,
    4);

  tft.drawString(
    "VISION",
    SCREEN_W
   / 2,
    115,
    4);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);

  tft.drawString(
    "DISPLAY       OK",
    SCREEN_W
   / 2,
    175,
    2);


  tft.setTextColor(TFT_YELLOW, TFT_BLACK);

  tft.drawString(
    "MLX90640      ...",
    SCREEN_W
   / 2,
    205,
    2);


  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);

  tft.drawString(
    "V1.0",
    SCREEN_W
   / 2,
    280,
    2);
}


//                                  Sensor status on startup screen

void showSensorStatus(bool success) {
  tft.fillRect(
    40,
    190,
    SCREEN_W
   - 80,
    30,
    TFT_BLACK);


  tft.setTextDatum(MC_DATUM);


  if (success) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);

    tft.drawString(
      "MLX90640      OK",
      SCREEN_W
     / 2,
      205,
      2);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);

    tft.drawString(
      "MLX90640      ERROR",
      SCREEN_W
     / 2,
      205,
      2);
  }
}

//                             Get data from mlx sensor

bool readThermalFrame() {
  int status = mlx.getFrame(frame.pixels);

    if (status < 0)
    {
        Serial.print("MLX90640 frame read failed, status: ");
        Serial.println(status);

        return false;
    }
// covering dead sensors
frame.pixels[258] = averageNeighbors(258);
frame.pixels[601] = averageNeighbors(601);

  frame.minTemp = frame.pixels[0];
  frame.maxTemp = frame.pixels[0];

  for (int i = 1; i < PIXEL_COUNT; i++) {

    float temperature = frame.pixels[i];

    if (temperature < frame.minTemp) {
      frame.minTemp = temperature;
    }

    if (temperature > frame.maxTemp) {
      frame.maxTemp = temperature;
    }
  }

  return true;
}

//                    preparing thermal data

void interpolateFrame()
{
    for (int y = 0; y < RENDER_H; y++)
    {
        // Position inside the original sensor grid
        float sensorY = (float)y * (GRID_H - 1) / (RENDER_H - 1);

        int y0 = (int)sensorY;
        int y1 = min(y0 + 1, GRID_H - 1);

        float fy = sensorY - y0;

        for (int x = 0; x < RENDER_W; x++)
        {
            float sensorX = (float)x * (GRID_W - 1) / (RENDER_W - 1);

            int x0 = (int)sensorX;
            int x1 = min(x0 + 1, GRID_W - 1);

            float fx = sensorX - x0;

            // Four surrounding sensor pixels
            float topLeft     = frame.pixels[y0 * GRID_W + x0];
            float topRight    = frame.pixels[y0 * GRID_W + x1];
            float bottomLeft  = frame.pixels[y1 * GRID_W + x0];
            float bottomRight = frame.pixels[y1 * GRID_W + x1];

            // Interpolate horizontally
            float top = topLeft + (topRight - topLeft) * fx;
            float bottom = bottomLeft + (bottomRight - bottomLeft) * fx;

            // Interpolate vertically
            float temperature = top + (bottom - top) * fy;

            renderFrame[y * RENDER_W + x] = temperature;
        }
    }
}

float normalizeTemp(float temp) {
  float range = frame.maxTemp - frame.minTemp;
  if (range < 0.1) range = 0.1;  // Prevent division by zero
  float v = (temp - frame.minTemp) / range;
  return constrain(v, 0.0, 1.0);
}

// float normalizeTemp(float temp)
// {
//     const float HUMAN_MIN_TEMP = 15.0;
//     const float HUMAN_MAX_TEMP = 40.0;

//     float v = (temp - HUMAN_MIN_TEMP) /
//               (HUMAN_MAX_TEMP - HUMAN_MIN_TEMP);

//     return constrain(v, 0.0, 1.0);
// }

void renderInterpolatedFrame()
{
    const int pixelW = SCREEN_W / RENDER_W;  // 3
    const int pixelH = SCREEN_H / RENDER_H;  // 5

    for (int y = 0; y < RENDER_H; y++)
    {
        for (int x = 0; x < RENDER_W; x++)
        {
            float temperature = renderFrame[y * RENDER_W + x];

            // Convert temperature into 0.0 - 1.0
            float value = normalizeTemp(temperature);

            uint8_t r;
            uint8_t g;
            uint8_t b;

            /*
             * Thermal palette:
             *
             * 0.00       0.20       0.40       0.60       0.80       1.00
             *   BLUE ─── CYAN ───── GREEN ─── YELLOW ─── ORANGE ─── RED
             */

            if (value < 0.20)
            {
                // BLUE → CYAN
                float t = value / 0.20;

                r = 0;
                g = (uint8_t)(255.0 * t);
                b = 255;
            }
            else if (value < 0.40)
            {
                // CYAN → GREEN
                float t = (value - 0.20) / 0.20;

                r = 0;
                g = 255;
                b = (uint8_t)(255.0 * (1.0 - t));
            }
            else if (value < 0.60)
            {
                // GREEN → YELLOW
                float t = (value - 0.40) / 0.20;

                r = (uint8_t)(255.0 * t);
                g = 255;
                b = 0;
            }
            else if (value < 0.80)
            {
                // YELLOW → ORANGE
                float t = (value - 0.60) / 0.20;

                r = 255;
                g = (uint8_t)(255.0 - (90.0 * t));
                b = 0;
            }
            else
            {
                // ORANGE → RED
                float t = (value - 0.80) / 0.20;

                r = 255;
                g = (uint8_t)(165.0 * (1.0 - t));
                b = 0;
            }

            uint16_t color = tft.color565(r, g, b);

            int screenX = x * pixelW;
            int screenY = y * pixelH;

            tft.fillRect(
                screenX,
                screenY,
                pixelW,
                pixelH,
                color
            );
        }
    }
}
// testing sensor data
// void printThermalFrame() {
//   Serial.println();
//   Serial.println("========== THERMAL FRAME ==========");

//   for (int row = 0; row < GRID_H; row++) {
//     for (int col = 0; col < GRID_W; col++) {
//       int index = row * GRID_W + col;
    
//       Serial.print(frame.pixels[index], 1);
//       Serial.print("\t");
//     }

//     Serial.println();
//   }

//   Serial.println("===================================");
//   Serial.print("Minimum: ");
//   Serial.print(frame.minTemp, 2);
//   Serial.println(" °C");

//   Serial.print("Maximum: ");
//   Serial.print(frame.maxTemp, 2);
//   Serial.println(" °C");

//   Serial.println();
// }

//                          avarage temp for filling dead sensors

float averageNeighbors(int index)
{
    int row = index / GRID_W;
    int col = index % GRID_W;

    float sum = 0.0;
    int count = 0;

    // Left
    if (col > 0 && !isnan(frame.pixels[index - 1])) {
        sum += frame.pixels[index - 1];
        count++;
    }

    // Right
    if (col < GRID_W - 1 && !isnan(frame.pixels[index + 1])) {
        sum += frame.pixels[index + 1];
        count++;
    }

    // Above
    if (row > 0 && !isnan(frame.pixels[index - GRID_W])) {
        sum += frame.pixels[index - GRID_W];
        count++;
    }

    // Below
    if (row < GRID_H - 1 && !isnan(frame.pixels[index + GRID_W])) {
        sum += frame.pixels[index + GRID_W];
        count++;
    }

    if (count == 0) {
        return 0.0;
    }

    return sum / count;
}

void setup() {
  Serial.begin(115200);

  delay(200);

  Serial.println();
  Serial.println("      CHEAP THERMAL VISION V1");

  initI2C();
  initDisplay();
  showStartupScreen();

  bool sensorReady = initThermalSensor();
  showSensorStatus(sensorReady);

    delay(2000);
//   if (readThermalFrame()) {
//     printThermalFrame();
//   } else {
//     Serial.println("Failed to read initial thermal frame!");
//   }
 }

void loop() {

      if (readThermalFrame())
{
    interpolateFrame();
    renderInterpolatedFrame();
}
}