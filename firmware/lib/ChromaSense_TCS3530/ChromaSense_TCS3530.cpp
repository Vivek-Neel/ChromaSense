// =============================================================================
// ChromaSense_TCS3530.cpp
// I2C driver for the AMS OSRAM TCS3530 XYZ tristimulus sensor.
//
// OTH Regensburg | Faculty ANK | Supervisor: Prof. Johannes Wild
// =============================================================================

#include "ChromaSense_TCS3530.h"

// -- Lifecycle -----------------------------------------------------------------

bool ChromaSense_TCS3530::begin(uint8_t sda, uint8_t scl, uint32_t freq,
                                uint16_t sampleCount, uint8_t gainCode) {
    Wire.begin(sda, scl);
    Wire.setClock(freq);
    Wire.setTimeOut(100);

    if (!isConnected()) {
        _initialised = false;
        return false;
    }

    // Wait out POR init (PON_INIT clears after ~300us). [DS p.26]
    unsigned long start = millis();
    while (millis() - start < 20) {
        uint8_t osc = readRegister(TCS3530_REG_OSCEN);
        if (osc != 0xFF && !(osc & TCS3530_OSCEN_PON_INIT)) break;
        delay(1);
    }

    // Enable oscillator; clocked registers are inaccessible until then. [DS p.26]
    if (!writeRegister(TCS3530_REG_OSCEN, TCS3530_OSCEN_ENABLE)) return false;

    start = millis();
    bool oscReady = false;
    while (millis() - start < 20) {
        uint8_t osc = readRegister(TCS3530_REG_OSCEN);
        if (osc != 0xFF && (osc & TCS3530_OSCEN_STATUS)) { oscReady = true; break; }
        delay(1);
    }
    if (!oscReady) return false;

    // Configure before PON. [DS p.26]
    if (!writeRegister(TCS3530_REG_CFG4, TCS3530_FIFO_24BIT)) return false;
    if (!writeRegister(TCS3530_REG_MEAS_MODE1, TCS3530_MEAS_MODE1_24BIT)) return false;

    // Restrict FIFO writes to X,Y,Z,IR (modulators 0-3). Clear bit7, keep
    // the default width nibble. This gives the confirmed 12-byte frame.
    if (!writeRegister(TCS3530_REG_MOD_FIFO_CFG4, 0x0F)) return false;  // HgL off
    if (!writeRegister(TCS3530_REG_MOD_FIFO_CFG5, 0x0F)) return false;  // HgH off
    if (!writeRegister(TCS3530_REG_MOD_FIFO_CFG6, 0x0F)) return false;  // Clear off
    if (!writeRegister(TCS3530_REG_MOD_FIFO_CFG7, 0x0F)) return false;  // Flicker off

    // Disable AGC so our fixed gain holds. MEAS_SEQR_AGC resets to 0xFF. [DS p.71]
    if (!writeRegister(TCS3530_REG_SEQR_AGC, 0x00)) return false;

    if (!setSampleTime(255))          return false;   // ~100ms with count=280
    if (!setGain(gainCode))           return false;
    if (!setSampleCount(sampleCount)) return false;   // writes 0x87/0x88

    // Verify critical writes landed (an I2C ACK does not guarantee the value).
    if (getGainRegister() != gainCode) return false;
    if (readRegister(TCS3530_REG_CFG4) != TCS3530_FIFO_24BIT) return false;
    if (readRegister(TCS3530_REG_MEAS_MODE1) != TCS3530_MEAS_MODE1_24BIT) return false;

    // PON, then PON|AEN (recommended two-write sequence). [SMG s.4.2]
    if (!writeRegister(TCS3530_REG_ENABLE, TCS3530_ENABLE_PON)) return false;
    delay(2);
    if (!writeRegister(TCS3530_REG_ENABLE,
                       TCS3530_ENABLE_PON | TCS3530_ENABLE_AEN)) return false;

    // Clear stale STATUS and FIFO so the first read() starts frame-aligned.
    writeRegister(TCS3530_REG_STATUS, 0xFF);
    writeRegister(TCS3530_REG_CONTROL, TCS3530_CONTROL_FIFO_CLR);

    _initialised  = true;
    _lastGainCode = gainCode;
    return true;
}

void ChromaSense_TCS3530::restartMeasurement() {
    writeRegister(TCS3530_REG_ENABLE, 0x00);
    writeRegister(TCS3530_REG_CONTROL, TCS3530_CONTROL_FIFO_CLR);
    writeRegister(TCS3530_REG_STATUS, 0xFF);
    delay(2);
    writeRegister(TCS3530_REG_ENABLE, TCS3530_ENABLE_PON);
    delay(2);
    writeRegister(TCS3530_REG_ENABLE, TCS3530_ENABLE_PON | TCS3530_ENABLE_AEN);
}

// -- Data Acquisition ----------------------------------------------------------

TCS3530_data ChromaSense_TCS3530::read() {
    TCS3530_data d;
    d.X = d.Y = d.Z = d.IR = 0;
    d.saturated = false;
    d.valid     = false;

    if (!_initialised) return d;

    // Corruption check before touching FIFO_DATA. A corrupted FIFO would also
    // break frame alignment, so we clear and restart. [SMG s.4.5.1]
    uint8_t flags = _fifoFlags();
    if (flags & (TCS3530_FIFO_OVERFLOW | TCS3530_FIFO_UNDERFLOW)) {
        restartMeasurement();
        return d;
    }

    if (!_waitDataReady()) return d;

    // Drop whole stale frames to return the freshest. The FIFO holds only whole
    // 12-byte frames and is cleared frame-aligned in begin(), so reading in
    // 12-byte units keeps alignment.
    uint16_t level = _fifoLevel();
    int guard = 0;
    while (level >= 2 * TCS3530_FRAME_BYTES && guard++ < 120) {
        Wire.beginTransmission(TCS3530_I2C_ADDR);
        Wire.write(TCS3530_REG_FIFO);
        if (Wire.endTransmission(false) != 0) return d;
        uint8_t got = Wire.requestFrom((uint8_t)TCS3530_I2C_ADDR,
                                       (uint8_t)TCS3530_FRAME_BYTES);
        for (uint8_t i = 0; i < got; i++) Wire.read();
        level = _fifoLevel();
    }

    Wire.beginTransmission(TCS3530_I2C_ADDR);
    Wire.write(TCS3530_REG_FIFO);
    if (Wire.endTransmission(false) != 0) return d;

    uint8_t n = Wire.requestFrom((uint8_t)TCS3530_I2C_ADDR,
                                 (uint8_t)TCS3530_FRAME_BYTES);
    if (n < TCS3530_FRAME_BYTES) return d;

    uint8_t buf[TCS3530_FRAME_BYTES];
    for (int i = 0; i < TCS3530_FRAME_BYTES; i++) buf[i] = Wire.read();

    // 24-bit little-endian: byte0 = low, byte1 = medium, byte2 = high.
    d.X  = (uint32_t)buf[0]  | ((uint32_t)buf[1]  << 8) | ((uint32_t)buf[2]  << 16);
    d.Y  = (uint32_t)buf[3]  | ((uint32_t)buf[4]  << 8) | ((uint32_t)buf[5]  << 16);
    d.Z  = (uint32_t)buf[6]  | ((uint32_t)buf[7]  << 8) | ((uint32_t)buf[8]  << 16);
    d.IR = (uint32_t)buf[9]  | ((uint32_t)buf[10] << 8) | ((uint32_t)buf[11] << 16);

    if (d.X  == TCS3530_SAT24_ANALOG || d.X  == TCS3530_SAT24_DIGITAL ||
        d.Y  == TCS3530_SAT24_ANALOG || d.Y  == TCS3530_SAT24_DIGITAL ||
        d.Z  == TCS3530_SAT24_ANALOG || d.Z  == TCS3530_SAT24_DIGITAL ||
        d.IR == TCS3530_SAT24_ANALOG || d.IR == TCS3530_SAT24_DIGITAL) {
        d.saturated = true;
    }

    d.valid = true;
    return d;
}

// -- Gain Control --------------------------------------------------------------

bool ChromaSense_TCS3530::setGain(uint8_t gainCode) {
    uint8_t packed = (gainCode << 4) | gainCode;

    Wire.beginTransmission(TCS3530_I2C_ADDR);
    Wire.write(TCS3530_REG_GAINX_0);     // auto-increments 0xC0-0xC3
    Wire.write(packed);
    Wire.write(packed);
    Wire.write(packed);
    Wire.write(packed);
    bool ok = (Wire.endTransmission() == 0);

    if (ok) _lastGainCode = gainCode;
    return ok;
}

uint8_t ChromaSense_TCS3530::getGainRegister() {
    uint8_t v = readRegister(TCS3530_REG_GAINX_0);
    if (v == 0xFF) return 0xFF;
    return v & 0x0F;
}

// -- Integration Time ----------------------------------------------------------

bool ChromaSense_TCS3530::setSampleCount(uint16_t sampleCount) {
    sampleCount &= 0x07FF;   // 11-bit

    Wire.beginTransmission(TCS3530_I2C_ADDR);
    Wire.write(TCS3530_REG_ALS_NR_S0);                  // 0x87, increments to 0x88
    Wire.write((uint8_t)(sampleCount & 0xFF));          // low  [7:0]
    Wire.write((uint8_t)((sampleCount >> 8) & 0x07));   // high [10:8]
    return (Wire.endTransmission() == 0);
}

bool ChromaSense_TCS3530::setSampleTime(uint16_t sampleTime) {
    sampleTime &= 0x07FF;   // 11-bit

    // 0x83 bits[7:5] = SAMPLE_TIME[2:0]; bits[4:0] preserved.
    // 0x84 bits[7:0] = SAMPLE_TIME[10:3]. [DS p.29]
    uint8_t lowBits  = (uint8_t)(sampleTime & 0x07);
    uint8_t highBits = (uint8_t)((sampleTime >> 3) & 0xFF);

    uint8_t cur = readRegister(TCS3530_REG_SAMPLE_T0);
    if (cur == 0xFF) return false;
    uint8_t newT0 = (cur & 0x1F) | (lowBits << 5);

    bool ok = writeRegister(TCS3530_REG_SAMPLE_T0, newT0);
    ok = ok && writeRegister(TCS3530_REG_SAMPLE_T1, highBits);
    return ok;
}

// -- Diagnostics ---------------------------------------------------------------

uint8_t ChromaSense_TCS3530::readRegister(uint8_t reg) {
    Wire.beginTransmission(TCS3530_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFF;
    uint8_t n = Wire.requestFrom((uint8_t)TCS3530_I2C_ADDR, (uint8_t)1);
    if (n < 1) return 0xFF;
    return Wire.read();
}

bool ChromaSense_TCS3530::writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(TCS3530_I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return (Wire.endTransmission() == 0);
}

bool ChromaSense_TCS3530::isConnected() {
    return readRegister(TCS3530_REG_ID) == TCS3530_DEVICE_ID;
}

// -- Private -------------------------------------------------------------------

bool ChromaSense_TCS3530::_waitDataReady() {
    unsigned long start = millis();
    while (millis() - start < 200) {
        if (_fifoLevel() >= TCS3530_FRAME_BYTES) return true;
        delay(2);
    }
    return false;
}

uint16_t ChromaSense_TCS3530::_fifoLevel() {
    // STATUS0 (0xFC) MUST be read before STATUS1 (0xFD) for a consistent latch.
    uint8_t s0 = readRegister(TCS3530_REG_FIFO_STAT0);
    uint8_t s1 = readRegister(TCS3530_REG_FIFO_STAT1);
    if (s0 == 0xFF && s1 == 0xFF) return 0;
    return ((uint16_t)s0 << 3) | (s1 & 0x07);
}

uint8_t ChromaSense_TCS3530::_fifoFlags() {
    (void)readRegister(TCS3530_REG_FIFO_STAT0);
    uint8_t s1 = readRegister(TCS3530_REG_FIFO_STAT1);
    if (s1 == 0xFF) return 0;
    return s1 & (TCS3530_FIFO_OVERFLOW | TCS3530_FIFO_UNDERFLOW);
}
