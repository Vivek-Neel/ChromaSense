// =============================================================================
// ChromaSense_TCS3530.h
// I2C driver for the AMS OSRAM TCS3530 XYZ tristimulus sensor.
//
// OTH Regensburg | Faculty ANK | Supervisor: Prof. Johannes Wild
//
// Reads ALS data via the FIFO in 24-bit format. Single ALS step, four
// channels (X,Y,Z,IR) write-enabled, ALS-status and gain FIFO writes
// disabled, giving a 12-byte frame: four little-endian 24-bit channels,
// no trailing step byte (note: differs from SMG Table 5, which assumes
// status/gain writes enabled).
//
// References: TCS3530 datasheet DS001080 v3-00, ams-OSRAM Settings and
// Measurement Guide v0-01. Cited inline as [DS p.XX] / [SMG s.X.Y].
// =============================================================================

#ifndef CHROMASENSE_TCS3530_H
#define CHROMASENSE_TCS3530_H

#include <Arduino.h>
#include <Wire.h>

// -- I2C Address ---------------------------------------------------------------
#define TCS3530_I2C_ADDR        0x39    // Fixed, no address pin       [DS p.17]

// -- Device Identification -----------------------------------------------------
#define TCS3530_REG_ID          0x92    // Device ID                   [DS p.35]
#define TCS3530_DEVICE_ID       0x68    // Expected ID value           [DS p.35]

// -- Core Control Registers ----------------------------------------------------
#define TCS3530_REG_OSCEN       0x7F    // Oscillator enable           [DS p.26]
#define TCS3530_REG_ENABLE      0x80    // Power on / ALS enable       [DS p.26]
#define TCS3530_REG_MEAS_MODE0  0x81    // ALS status FIFO write        [DS p.27]
#define TCS3530_REG_MEAS_MODE1  0x82    // ALS_MSB_POSITION / gain FIFO [DS p.28]
#define TCS3530_REG_SAMPLE_T0   0x83    // Sample time low             [DS p.29]
#define TCS3530_REG_SAMPLE_T1   0x84    // Sample time high            [DS p.29]
#define TCS3530_REG_ALS_NR_S0   0x87    // ALS sample count low        [DS p.31]
#define TCS3530_REG_ALS_NR_S1   0x88    // ALS sample count high       [DS p.31]

// -- Configuration Registers ---------------------------------------------------
#define TCS3530_REG_CFG4        0xA5    // FIFO data width             [DS p.45]

// -- Gain Registers ------------------------------------------------------------
// Four registers, each two 4-bit gain nibbles for 8 modulators. [DS p.56-63]
#define TCS3530_REG_GAINX_0     0xC0    // Modulators 0,1 gain
#define TCS3530_REG_GAINX_1     0xC1    // Modulators 2,3 gain
#define TCS3530_REG_GAINX_2     0xC2    // Modulators 4,5 gain
#define TCS3530_REG_GAINX_3     0xC3    // Modulators 6,7 gain

// -- Measurement Sequencer -----------------------------------------------------
#define TCS3530_REG_SEQR_AGC    0xDD    // AGC predict/asat patterns   [DS p.71]

// -- FIFO Channel Write-Enable Config ------------------------------------------
// MOD_FIFO_DATA_CFGx: bit7 = ALS write enable, bits[3:0] = width nibble.
#define TCS3530_REG_MOD_FIFO_CFG4  0xF8 // HgL    channel
#define TCS3530_REG_MOD_FIFO_CFG5  0xF9 // HgH    channel
#define TCS3530_REG_MOD_FIFO_CFG6  0xFA // Clear  channel
#define TCS3530_REG_MOD_FIFO_CFG7  0xFB // Flicker channel

// -- Status Registers ----------------------------------------------------------
#define TCS3530_REG_STATUS      0x9B    // General device status       [DS p.37]
#define TCS3530_REG_STATUS2     0x9C    // Saturation flags            [DS p.38]
#define TCS3530_REG_FIFO_STAT0  0xFC    // FIFO level [10:3]           [DS p.88]
#define TCS3530_REG_FIFO_STAT1  0xFD    // FIFO level [2:0] + ovf/udf  [DS p.89]
#define TCS3530_REG_CONTROL     0xBC    // FIFO_CLR control bit        [DS p.54]

// -- FIFO Data Register --------------------------------------------------------
#define TCS3530_REG_FIFO        0xFF    // Sequential FIFO read        [DS p.91]

// -- ENABLE Register Bit Masks -------------------------------------------------
#define TCS3530_ENABLE_PON      0x01
#define TCS3530_ENABLE_AEN      0x02

// -- OSCEN Register Bit Masks --------------------------------------------------
#define TCS3530_OSCEN_ENABLE    0x01
#define TCS3530_OSCEN_STATUS    0x02
#define TCS3530_OSCEN_PON_INIT  0x04

// -- FIFO_STATUS1 Bit Masks ----------------------------------------------------
#define TCS3530_FIFO_OVERFLOW   0x80
#define TCS3530_FIFO_UNDERFLOW  0x40

// -- CONTROL Register Bit Masks ------------------------------------------------
#define TCS3530_CONTROL_FIFO_CLR  0x02

// -- CFG4 FIFO Width Values ----------------------------------------------------
#define TCS3530_FIFO_16BIT      0x00
#define TCS3530_FIFO_24BIT      0x01
#define TCS3530_FIFO_32BIT      0x03

// -- MEAS_MODE1: ALS_MSB_POSITION ----------------------------------------------
// 24-bit FIFO format REQUIRES als_msb_position = 8 (reset is 12). Bits 7,5 off.
#define TCS3530_MEAS_MODE1_24BIT  0x08

// -- FIFO Frame Geometry -------------------------------------------------------
// 4 channels (X,Y,Z,IR) x 3 bytes, little-endian, no step byte = 12 bytes
// per frame. Channels 4-7 are disabled for FIFO write in begin().
#define TCS3530_CHANNELS          4
#define TCS3530_BYTES_PER_CH      3
#define TCS3530_FRAME_BYTES       (TCS3530_CHANNELS * TCS3530_BYTES_PER_CH)  // 12

// 24-bit saturation sentinel codes. [SMG s.4.6]
#define TCS3530_SAT24_ANALOG    0xFFFFFFUL
#define TCS3530_SAT24_DIGITAL   0xFFFFFEUL

// -- Gain Code Values ----------------------------------------------------------
#define TCS3530_GAIN_0_5X       0x00
#define TCS3530_GAIN_1X         0x01
#define TCS3530_GAIN_2X         0x02
#define TCS3530_GAIN_4X         0x03
#define TCS3530_GAIN_8X         0x04
#define TCS3530_GAIN_16X        0x05
#define TCS3530_GAIN_32X        0x06
#define TCS3530_GAIN_64X        0x07
#define TCS3530_GAIN_128X       0x08
#define TCS3530_GAIN_256X       0x09

// -- Read Result Structure -----------------------------------------------------
struct TCS3530_data {
    uint32_t X;           // CIE X channel, 24-bit
    uint32_t Y;           // CIE Y channel, 24-bit (proportional to lux)
    uint32_t Z;           // CIE Z channel, 24-bit
    uint32_t IR;          // IR channel, 24-bit
    bool     saturated;   // true if any channel hit a saturation sentinel
    bool     valid;       // true if a frame was read successfully
};

// -- Driver Class --------------------------------------------------------------
class ChromaSense_TCS3530 {
public:
    bool begin(uint8_t sda, uint8_t scl, uint32_t freq,
               uint16_t sampleCount, uint8_t gainCode);

    TCS3530_data read();

    bool    setGain(uint8_t gainCode);
    uint8_t getGainRegister();
    bool    setSampleCount(uint16_t sampleCount);
    bool    setSampleTime(uint16_t sampleTime);

    uint8_t readRegister(uint8_t reg);
    bool    writeRegister(uint8_t reg, uint8_t value);
    bool    isConnected();
    void    restartMeasurement();

private:
    bool    _initialised  = false;
    uint8_t _lastGainCode = TCS3530_GAIN_2X;

    bool     _waitDataReady();
    uint16_t _fifoLevel();
    uint8_t  _fifoFlags();
};

#endif // CHROMASENSE_TCS3530_H
