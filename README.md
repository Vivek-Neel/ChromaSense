# ChromaSense

**A wireless colorimeter built from the datasheet up — custom PCB, custom sensor driver, factory-calibrated colour pipeline, and a live CIE 1931 dashboard — for about 1–4% of the cost of a commercial instrument.**

![ChromaSense device showing live illuminance, CCT and CIE chromaticity readings next to the custom breakout board](assets/hero.jpg)

---

## What this project proves

- **I can take a sensor from datasheet to working instrument.** TCS3530 → custom KiCad breakout → ESP32 firmware → calibrated colour values → wireless GUI. No module, no example sketch.
- **I write my own driver when the vendor library is wrong.** The off-the-shelf library had three separate faults that combined to produce plausible-looking garbage. I diagnosed each one and wrote a replacement from the datasheet. ([Full write-up below.](#the-bug-that-mattered-a-driver-that-looked-fine-and-wasnt))
- **I apply real metrological calibration, not a fudge factor.** Per-sensor ams-OSRAM factory calibration: integration-time normalisation → per-channel gain normalisation → 3×4 correction matrix.
- **I state my limitations.** Lux scale factor is uncharacterised; narrow-band sources fall outside the calibrated range. [Both documented.](#limitations)

## At a glance

| | |
|---|---|
| **Measures** | Illuminance (lux), CCT (McCamy), CIE 1931 *xy* chromaticity |
| **Sensor** | ams-OSRAM TCS35303-2M XYZ tristimulus, 24-bit channel data |
| **Board** | Custom 2-layer breakout, 25 × 30 mm, KiCad → JLCPCB, DRC clean |
| **MCU** | M5Stack Core2 (ESP32), I²C @ 400 kHz |
| **Link** | Bluetooth SPP, JSON telemetry @ 10 Hz |
| **GUI** | Python / PyQt5, CIE 1931 diagram computed from CIE colour-matching functions, CSV logging |
| **Cost** | **≈ €62 per unit** vs €1,470–6,000 for commercial meters |

---

## Architecture

```
  ┌──────────────────────┐
  │  TCS35303-2M         │  XYZ tristimulus sensor
  │  (ChromaSense        │  1.8 V via AP2112K LDO
  │   Breakout v1.1)     │
  └──────────┬───────────┘
             │  I²C  @ 400 kHz   (SDA 32 / SCL 33, Grove HY2.0-4P)
             ▼
  ┌──────────────────────┐
  │  M5Stack Core2       │  ESP32 · AXP2101 PMIC · IPS LCD
  │                      │
  │  ChromaSense_TCS3530 │  custom 24-bit driver
  │          ↓           │
  │  factory calibration │  ATIME norm → AGAIN norm → 3×4 matrix
  │          ↓           │
  │  lux · CCT · x,y     │  photopic weighting · McCamy
  └──────────┬───────────┘
             │  Bluetooth SPP — JSON @ 10 Hz
             │  {lux, cct, x, y, X, Y, Z, mode, batt, dark}
             ▼
  ┌──────────────────────┐
  │  Python GUI (PyQt5)  │  live CIE 1931 diagram · CSV logging
  └──────────────────────┘
```

---

## Hardware

<img src="assets/pcb-photo.jpg" width="45%" alt="Assembled ChromaSense Breakout v1.1"> <img src="assets/pcb-layout.png" width="45%" alt="KiCad PCB layout">

The breakout exists because the TCS3530 ships in an OLGA package with a 1.8 V supply rail — it cannot be wired to a 3.3 V dev board directly. The board carries the sensor, its own LDO, decoupling, and a Grove connector so it drops straight onto the Core2's I²C port.

| Ref | Component | Part | Purpose |
|-----|-----------|------|---------|
| U1 | XYZ tristimulus sensor | TCS35303-2M | Ambient light / colour measurement |
| U2 | LDO regulator | AP2112K-1.8TRG1 | 1.8 V sensor supply (SOT-23-5) |
| C1–C4 | MLCC 0402 | 4.7 µF / 1 µF, X5R, 10 V | LDO and sensor decoupling |
| J1 | Connector | Grove HY2.0-4P | I²C to M5Stack Core2 |

**PCB:** 2-layer, 25 × 30 mm · power traces 0.3 mm / signal traces 0.15 mm · GND copper pour on B.Cu · DRC 0 errors · fabricated at JLCPCB.

Optical design follows the ams-OSRAM TCS3530 Optical Design Guide: 3.12 mm aperture, 0.20 ± 0.15 mm air gap.

---

## Firmware

PlatformIO · `espressif32@6.13.0` · board `m5stack-core2` · M5Unified.

**Custom driver — `firmware/lib/ChromaSense_TCS3530`.** Written from the TCS3530 datasheet (DS001080 v3-00) after the available vendor library proved unusable for this application (see below). Handles oscillator bring-up, 24-bit FIFO configuration, gain and integration-time control, and FIFO overflow/underflow recovery. Every register reference in the header is cited to a datasheet page number.

**Calibration pipeline.** Per-sensor ams-OSRAM factory calibration, applied in three stages:

1. **ATIME normalisation** — scale raw counts from the factory reference integration time (20 ms) to the operating setting.
2. **AGAIN normalisation** — per-channel gain correction to a 128× reference, using the `GCORR` coefficients measured at 0.5× / 2× / 16×.
3. **Matrix correction** — 3×4 `CAL_A` matrix mapping corrected sensor counts to CIE XYZ.

From XYZ: chromaticity `x = X/(X+Y+Z)`, `y = Y/(X+Y+Z)`; CCT via the McCamy cubic approximation; illuminance from the photopic-weighted Y channel.

> The calibration coefficients for this specific sensor (UID `557D0C06B8`) are included in `firmware/src/main.cpp`, with my supervisor's agreement — published here only as applied numeric values in source, never as the vendor's calibration file, documentation, or driver, none of which are redistributed in this repository.

**Also implemented:** on-device dashboard on the Core2 IPS LCD, battery monitoring via AXP2101, and a dark mode (3-second button hold or a `DARK`/`LIGHT` command from the GUI) that blanks the backlight for low-light measurement without touching the I²C bus — firmware state is echoed in every JSON packet so the GUI stays in sync regardless of which side triggered it.

---

## Python GUI

![ChromaSense PyQt5 dashboard with live CIE 1931 chromaticity diagram](assets/gui-dashboard.png)

PyQt5 dashboard: CIE 1931 chromaticity diagram with a live position marker, telemetry panels for illuminance, CCT, chromaticity, raw channel counts and battery, plus CSV logging for measurement sessions.

**The chromaticity diagram is computed, not an image.** The spectral locus is rendered from the CIE 1931 colour-matching functions via `scipy.interpolate.CubicSpline` at 0.1 nm steps — `np.interp` is piecewise-linear and visibly facets the curve at the green apex, so a proper spline was needed. The gamut fill inside the locus is computed per-pixel from an XYZ → linear-sRGB transform (IEC 61966-2-1) with correct gamma companding, not a pre-rendered picture. The result is self-contained: no third-party chart image, no licensing question.

Other implementation notes worth the trouble:

- **10 Hz without stutter.** Qt re-parses a stylesheet on every `setStyleSheet` call. The CCT and battery labels use colour gradients, so naive updates re-parsed twice per packet. Caching the last applied colour and skipping no-op updates removes it.
- **Serial I/O off the UI thread.** `bt_worker.py` runs the Bluetooth link in a `QThread` with a `send()` method for outbound commands.

---

## The bug that mattered: a driver that looked fine and wasn't

**Symptom.** With the first library I tried, every channel read close to zero regardless of light level, and changing the gain setting in software had no visible effect on the output.

**Why it was hard.** Nothing errored. The sensor acknowledged on I²C, the ID register read back correctly, and the output was a plausible — if suspiciously flat — stream of numbers. There was no exception to catch, only values that didn't respond the way they should have.

**Root cause — three separate faults in the library:**

1. **Wrong register addresses.** Gain was written to the wrong address, and the ALS sample-count register (`0x87`/`0x88`, 11-bit, sets the integration time) was also wrong. With the sample count never actually set, integration time stayed near zero and every channel read close to nothing — the dominant symptom, and the one that made the bug look like a wiring or power problem rather than a software one.
2. **16-bit truncation of 24-bit data.** The TCS3530's FIFO can be configured for 16, 24 or 32-bit channel width (`CFG4`) with the MSB alignment set separately (`ALS_MSB_POSITION`). Left at their reset defaults, the driver silently read only the top 16 bits of a 24-bit value — a loss of dynamic range that produces numbers which still look like data, not an error.
3. **Frame width depends on configuration, and the library assumed the wrong one.** The FIFO's frame size isn't fixed — it depends on which channels are write-enabled and whether ALS-status and gain bytes are included. My configuration (four channels, status/gain writes disabled) gives a 12-byte frame; the layout the library assumed corresponds to a different configuration. Getting this wrong misaligns every subsequent read, compounding the error with each frame.

**A fourth trap, during bring-up.** The oscillator enable bit (`OSCEN`, register `0x7F`) must be set and confirmed before any *clocked* register access works. But the device ID register is unclocked — it reads back correctly even with the oscillator off. The standard "is the sensor alive?" check gives a confident false positive while every other register is still inaccessible.

**Resolution.** I wrote `ChromaSense_TCS3530` from the datasheet: correct register addresses verified against datasheet page references, explicit 24-bit FIFO configuration, frame handling matched to my actual channel configuration, and an oscillator-ready wait before touching anything clocked. Gain and integration time then behaved as specified, and the factory calibration could be applied meaningfully — it cannot be, on truncated or misaligned data.

**What I took from it.** A library that compiles, enumerates, and returns believable numbers can still be wrong in every way that matters. A reading that should change and doesn't is a stronger signal than one that looks wrong. And the cheapest sanity check available — reading the ID register — was the one that gave a false pass.

---

## Cost

Per working unit, at the single-unit prices paid:

| Item | € |
|------|---|
| TCS35303-2M sensor | 8.50 |
| AP2112K-1.8TRG1 LDO | 0.45 |
| MLCC decoupling, 0402 (×4) | 0.20 |
| Grove HY2.0-4P connector | 0.35 |
| PCB, 2-layer (JLCPCB, 5-off, incl. shipping) | ≈ 3.00 |
| **Breakout subtotal** | **≈ 12.50** |
| M5Stack Core2 (ESP32 platform) | 50.00 |
| **Total per unit** | **≈ 62.50** |

For comparison:

| Instrument | Price |
|------------|-------|
| [Sekonic C-800 SpectroMaster](https://galaxus.at/en/s1/product/sekonic-c-800-spectromaster-flash-unit-battery-flash-accessories-11488106) | €1,470 incl. VAT |
| [Konica Minolta CL-200A Chroma Meter](https://store.imatest.com/minolta-cl-200a-chroma-meter.html) | $6,500 |

**This is not a claim of equivalence.** A C-800 is a 380–780 nm spectrometer with traceable calibration; ChromaSense is a tristimulus sensor with a per-device factory correction. What the comparison shows is that factory-calibrated tristimulus sensing puts usable CIE coordinates within reach at roughly 1–4% of instrument cost — which is the interesting engineering result, not a replacement for a reference meter.

---

## Limitations

Stated plainly, because they bound what the measurements mean:

- **Illuminance scale factor is uncharacterised.** The lux output has not yet been characterised against a reference lux meter with the optical stack installed. Relative readings are consistent; absolute readings are provisional.
- **Narrow-band sources fall outside the calibrated range.** The TCS3530's Z channel carries significant UV sensitivity — Z/Y ≈ 8.2 under diffuse sky versus ≈ 0.49 under direct sun. Monitor primaries and narrow-band LEDs sit outside the conditions the factory matrix was fitted for, and chromaticity for those sources should be treated as indicative.
- **No enclosure.** A 3D-printed housing was descoped for schedule reasons. Measurements are taken with the bare board, so stray light is uncontrolled.
- **Single-unit calibration.** The calibration in this repository is specific to one sensor (UID `557D0C06B8`). Another sensor requires its own factory calibration from ams-OSRAM.

---

## Repository structure

```
chromasense/
├── hardware/            KiCad project — schematic, layout, Gerbers,
│                        custom symbol and footprint libraries, STEP model
├── firmware/            PlatformIO project (ESP32)
│   ├── src/main.cpp
│   └── lib/ChromaSense_TCS3530/     custom sensor driver
├── python-gui/          PyQt5 dashboard
│   ├── main_window.py
│   ├── cie_widget.py
│   └── bt_worker.py
├── assets/              photographs and screenshots
└── requirements.txt
```

## Build

**Firmware**

```bash
cd firmware
pio run --target upload
```

Sensor calibration for UID `557D0C06B8` is already applied in `src/main.cpp`. Using a different TCS3530 requires its own factory calibration data from ams-OSRAM, substituted into the same constants.

**GUI**

```bash
cd python-gui
pip install -r ../requirements.txt
python main_window.py
```

Pair the Core2 over Bluetooth, select the SPP serial port in the GUI, and connect.

---

## Context

M.Eng. project, Electrical and Microsystems Engineering — **OTH Regensburg**, Faculty ANK, Summer Semester 2026.
Supervisor: Prof. Johannes Wild.

**Vivek Wilson Neelamkavil** · [LinkedIn](#) · [CV](#)

The TCS3530 is referenced throughout against its public datasheet (ams-OSRAM DS001080 v3-00); no ams-OSRAM documentation, software, or raw calibration data is redistributed in this repository.
