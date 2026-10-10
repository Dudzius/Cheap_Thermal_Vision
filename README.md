# Cheap Thermal Vision

A pocket-sized, battery-powered thermal camera built from two inexpensive parts: an **ESP32 "Cheap Yellow Display"** and an **MLX90640** infrared sensor array. Made for everyday home use: finding cold spots and drafts, checking body temperature, spotting overheating electronics, and peeking at cooking.

## Features

- **Live thermal image**: 32x24 sensor interpolated up to 256x192, with the correct 4:3 aspect ratio
- **Center-point temperature**: large readout with a crosshair on the image
- **Min / max temperature** of the scene
- **Calm auto-range**: outlier-resistant, with a minimum span and slow, smoothed changes, so colors stay readable instead of flashing
- **Color scale bar** with temperature labels
- **Tap to freeze**: touch the screen to hold the current frame, touch again to resume
- **Battery indicator** (optional, needs a small voltage divider)
- Automatic **dead pixel** correction
- Sensor reading runs in its own task, so the touch screen stays responsive
- Single file, easy to tweak: orientation, range behavior, refresh rate and more are `#define`s at the top

## Hardware

| Part | Notes |
|---|---|
| ESP32-2432S028 ("CYD") | 2.8" 240x320 ILI9341 TFT, resistive touch, ESP32 |
| MLX90640 breakout | 32x24 IR array, I2C. Narrow field-of-view variant works well |
| 18650 Li-ion cell | Single cell |
| 18650 charger + 5 V boost module | Charges the cell and powers the board at 5 V |
| Slide switch | Between the battery module and the board (power on/off) |
| 2 x 100 kΩ resistors | Optional, for battery level readout |

### Wiring

**Thermal sensor (I2C)**

| MLX90640 | CYD |
|---|---|
| VIN | 3V3 |
| GND | GND |
| SDA | GPIO 27 |
| SCL | GPIO 22 |

**Power**

```
18650 ──► charger / boost module ──► switch ──► 5 V in on the CYD
```

**Battery level (optional)**

The CYD cannot measure the battery by itself, and the 5 V output tells you nothing about the cell. Tap the cell's own positive terminal (**B+** on the module) with a divider:

```
B+ (battery +) ──[100k]──┬──[100k]── GND
                         │
                      GPIO 35
```

Then set `BATTERY_ENABLED` to `1` in the sketch. Use the real battery side, not the 5 V output. The divider draws roughly 20 µA, which is negligible.

## Software

### Requirements

- Arduino IDE (or PlatformIO) with the **ESP32** board package
- Libraries:
  - [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI)
  - [Adafruit MLX90640](https://github.com/adafruit/Adafruit_MLX90640)
  - Adafruit BusIO (installed with the library above)

### TFT_eSPI setup

Configure TFT_eSPI for the ESP32-2432S028 (ILI9341 driver, display pins and backlight on GPIO 21) in your `User_Setup.h`, or use a ready-made CYD setup file. If the display works with TFT_eSPI examples, it works here.

### Build & flash

1. Open `CheapThermalVision.ino`
2. Select an ESP32 Dev Module board
3. Upload

## Usage

- Power on with the switch, and the splash screen runs a sensor check
- Point at something; the crosshair marks the center-point reading
- **Tap anywhere** to freeze the frame, **tap again** to resume

## Configuration

Everything lives at the top of the sketch:

| Setting | What it does |
|---|---|
| `SCREEN_ROTATION`, `FLIP_X`, `FLIP_Y` | Orientation and mirroring |
| `RANGE_MIN_SPAN` | Narrowest allowed color scale (°C). Lower = more sensitive, higher = calmer |
| `RANGE_GROW` / `RANGE_SHRINK` | How fast the color scale widens / narrows |
| `USE_FIXED_RANGE`, `FIXED_RANGE_LO/HI` | Use a fixed temperature range instead of auto |
| `SENSOR_REFRESH`, `I2C_CLOCK` | Sensor speed. Lower both if you see read errors |
| `BATTERY_ENABLED`, `BATT_DIVIDER`, `BATT_CAL` | Battery indicator and calibration |
| `KNOWN_BAD_PIXELS` | Dead pixel indexes of your particular sensor |

## Good to know

- **Emissivity:** the sensor assumes a typical surface. Skin, food, plastic and painted surfaces read accurately. Shiny metal reads too cold.
- **Glass:** infrared does not pass through glass or most plastics, so you see the surface of the glass, not what is behind it.
- **Accuracy:** good for finding relative differences and rough values, not a calibrated measuring instrument.
- **Battery percentage** is estimated from cell voltage, so it moves a little with load and while charging.
- Every MLX90640 has its own dead pixels, so check your sensor and update `KNOWN_BAD_PIXELS`.

## Enclosure

A 3D-printable case is planned. Files will be added here when ready.
