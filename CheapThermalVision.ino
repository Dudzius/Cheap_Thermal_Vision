#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <Adafruit_MLX90640.h>

// ============================================================
// Hardware configuration
// ============================================================

#define I2C_SDA 27
#define I2C_SCL 22

#define SCREEN_WIDTH  270
#define SCREEN_HEIGHT 340


// ============================================================
// Objects
// ============================================================

TFT_eSPI tft = TFT_eSPI();
Adafruit_MLX90640 mlx;


// ============================================================
// Display initialization
// ============================================================

void initDisplay()
{
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    tft.init();

    tft.setRotation(0);

    tft.fillScreen(TFT_BLACK);
}


// ============================================================
// I2C initialization
// ============================================================

void initI2C()
{
    Wire.begin(I2C_SDA, I2C_SCL);

    Wire.setClock(400000);
}

// ============================================================
// MLX90640 initialization
// ============================================================

bool initThermalSensor()
{
    Serial.println("Initializing MLX90640...");

    // The default MLX90640 address is 0x33.
    if (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire))
    {
        Serial.println("MLX90640 initialization FAILED");

        return false;
    }

    Serial.println("MLX90640 detected");

    mlx.setMode(MLX90640_CHESS);
    mlx.setResolution(MLX90640_ADC_18BIT);
    mlx.setRefreshRate(MLX90640_4_HZ);

    Serial.println("MLX90640 configured");

    return true;
}

// ============================================================
// Startup screen
// ============================================================

void showStartupScreen()
{
    tft.fillScreen(TFT_BLACK);

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);

    tft.setTextDatum(MC_DATUM);

    tft.drawString(
        "CHEAP",
        SCREEN_WIDTH / 2,
        45,
        4
    );

    tft.drawString(
        "THERMAL",
        SCREEN_WIDTH / 2,
        80,
        4
    );

    tft.drawString(
        "VISION",
        SCREEN_WIDTH / 2,
        115,
        4
    );

    // Display status
    tft.setTextColor(TFT_GREEN, TFT_BLACK);

    tft.drawString(
        "DISPLAY       OK",
        SCREEN_WIDTH / 2,
        175,
        2
    );


    // Sensor status
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);

    tft.drawString(
        "MLX90640      ...",
        SCREEN_WIDTH / 2,
        205,
        2
    );


    // Version
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);

    tft.drawString(
        "V1.0",
        SCREEN_WIDTH / 2,
        280,
        2
    );
}


// ============================================================
// Sensor status on startup screen
// ============================================================

void showSensorStatus(bool success)
{
    tft.fillRect(
        40,
        190,
        SCREEN_WIDTH - 80,
        30,
        TFT_BLACK
    );


    tft.setTextDatum(MC_DATUM);


    if (success)
    {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);

        tft.drawString(
            "MLX90640      OK",
            SCREEN_WIDTH / 2,
            205,
            2
        );
    }
    else
    {
        tft.setTextColor(TFT_RED, TFT_BLACK);

        tft.drawString(
            "MLX90640      ERROR",
            SCREEN_WIDTH / 2,
            205,
            2
        );
    }
}

// ============================================================
// Arduino setup
// ============================================================

void setup()
{
    Serial.begin(115200);

    delay(200);

    Serial.println();
    Serial.println("==============================");
    Serial.println("      THERMAL VISION V1");
    Serial.println("==============================");


    // 1. Start I2C
    Serial.println("Initializing I2C...");
    initI2C();
    Serial.println("I2C ready");


    // 2. Start display
    Serial.println("Initializing display...");
    initDisplay();
    Serial.println("Display ready");


    // 3. Show startup UI
    showStartupScreen();


    // 4. Start thermal sensor
    bool sensorReady = initThermalSensor();


    // 5. Update startup UI
    showSensorStatus(sensorReady);


    if (sensorReady)
    {
        Serial.println();
        Serial.println("SYSTEM READY");
        Serial.println("==============================");
    }
    else
    {
        Serial.println();
        Serial.println("SYSTEM ERROR");
        Serial.println("Check MLX90640 connection.");
        Serial.println("==============================");
    }

}


// ============================================================
// Arduino main loop
// ============================================================

void loop()
{
    // Nothing yet.
}