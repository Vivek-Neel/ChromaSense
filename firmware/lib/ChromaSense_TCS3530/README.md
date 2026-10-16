# ChromaSense_TCS3530

Arduino/PlatformIO driver for the AMS OSRAM TCS3530 XYZ tristimulus sensor,
written for the ChromaSense remote colour sensor project at OTH Regensburg.

Replaces the `smplboards-TCS3530` library to fix the issues that prevented
accurate measurement:

1. **Correct register addresses.** Gain is written to `0xC0`–`0xC3` and ALS
   sample count to `0x87`/`0x88`. Earlier code used wrong addresses for both,
   which left the gain stuck and the ALS integration time near zero (all
   channels read ~0).
2. **24-bit FIFO reads.** CFG4 is set to 24-bit mode and `ALS_MSB_POSITION`
   to 8, so all three bytes per channel are read. The previous library left
   CFG4 at its 16-bit default and discarded the lower byte of each channel.
3. **Robust acquisition.** Frame-aligned 12-byte FIFO reads, FIFO
   overflow/underflow detection with automatic recovery, and write-back
   verification of the critical configuration registers.

AGC is disabled; the driver runs at a fixed, host-set gain. Saturation is
detected from the 24-bit sentinel codes (`0xFFFFFF` analog, `0xFFFFFE`
digital).

## FIFO Frame Layout

With a single ALS step, channels X,Y,Z,IR write-enabled, and ALS-status /
gain FIFO writes disabled, each FIFO frame is **12 bytes**: four channels,
3 bytes each, little-endian (low, medium, high), in order X, Y, Z, IR. There
is no trailing step byte in this configuration (the SMG Table 5 layout
assumes status/gain writes are enabled).

## Register Map Reference

Addresses verified against TCS3530 datasheet DS001080 v3-00 and confirmed on
hardware.

| Function             | Register          | Address      |
|----------------------|-------------------|--------------|
| Device ID            | ID                | 0x92 (=0x68) |
| Oscillator enable    | OSCEN             | 0x7F         |
| Power / ALS enable   | ENABLE            | 0x80         |
| ALS MSB position     | MEAS_MODE1        | 0x82         |
| Sample time          | SAMPLE_TIME       | 0x83 / 0x84  |
| ALS sample count     | ALS_NR_SAMPLES    | 0x87 / 0x88  |
| FIFO width           | CFG4              | 0xA5         |
| Gain (8 modulators)  | MOD_GAINX_0..3    | 0xC0 – 0xC3  |
| AGC patterns         | MEAS_SEQR_AGC     | 0xDD         |
| FIFO clear           | CONTROL           | 0xBC         |
| FIFO level + flags   | FIFO_STATUS0/1    | 0xFC / 0xFD  |
| FIFO data            | FIFO_DATA         | 0xFF         |

## Gain Codes

| Code | Gain  |
|------|-------|
| 0x00 | 0.5×  |
| 0x02 | 2×    |
| 0x05 | 16×   |

(Full table 0.5× to 256× in the header.)

## Basic Usage

```cpp
#include <ChromaSense_TCS3530.h>

ChromaSense_TCS3530 sensor;

void setup() {
    // sda, scl, freq, initial sample count, initial gain
    bool ok = sensor.begin(32, 33, 400000, 280, TCS3530_GAIN_2X);
}

void loop() {
    TCS3530_data d = sensor.read();
    if (d.valid && !d.saturated) {
        // d.X, d.Y, d.Z, d.IR are 24-bit values
    }
}
```

## Gain Switching

Gain and integration time can be changed at runtime with `setGain()` and
`setSampleCount()`. After changing either, clear the FIFO and allow one
integration period before trusting the next read, since in-flight frames
were measured at the previous setting.
