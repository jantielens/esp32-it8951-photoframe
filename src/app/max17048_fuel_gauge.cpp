#include "max17048_fuel_gauge.h"

#include "board_config.h"
#include "log_manager.h"

#include <Wire.h>

namespace {

static constexpr uint8_t kMax17048Addr = 0x36;

enum class Reg : uint8_t {
    VCELL = 0x02,
    SOC = 0x04,
    VERSION = 0x08,
    CRATE = 0x16,
};

static bool g_inited = false;
static bool g_available = false;

static bool i2c_read16(uint8_t addr, Reg reg, uint16_t* out) {
    if (!out) return false;

    Wire.beginTransmission(addr);
    Wire.write((uint8_t)reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    const uint8_t n = Wire.requestFrom((int)addr, 2);
    if (n != 2) return false;

    const uint8_t msb = Wire.read();
    const uint8_t lsb = Wire.read();
    *out = (uint16_t)((msb << 8) | lsb);
    return true;
}

static bool probe() {
    uint16_t version = 0;
    if (!i2c_read16(kMax17048Addr, Reg::VERSION, &version)) {
        return false;
    }

    // VERSION is not 0x0000 on real parts; treat 0 as suspicious but not fatal.
    // We only need a reliable "responds on I2C" signal.
    return true;
}

} // namespace

void max17048_init() {
    if (g_inited) return;
    g_inited = true;

#if HAS_FUEL_GAUGE
    // Use explicit pins to avoid depending on variant defaults.
    // FeatherS3[D] wiring: SDA=IO8, SCL=IO9.
    Wire.begin(FUEL_GAUGE_I2C_SDA_PIN, FUEL_GAUGE_I2C_SCL_PIN);
    Wire.setClock(400000);

    g_available = probe();
    if (g_available) {
        LOGI("Fuel", "MAX17048 detected");
    } else {
        LOGW("Fuel", "MAX17048 not detected");
    }
#else
    g_available = false;
#endif
}

bool max17048_available() {
    return g_available;
}

bool max17048_read(Max17048Reading* out) {
    if (!out) return false;
    if (!g_inited) {
        max17048_init();
    }
    if (!g_available) return false;

    uint16_t vcell = 0;
    uint16_t soc = 0;
    uint16_t crate = 0;

    if (!i2c_read16(kMax17048Addr, Reg::VCELL, &vcell)) return false;
    if (!i2c_read16(kMax17048Addr, Reg::SOC, &soc)) return false;
    if (!i2c_read16(kMax17048Addr, Reg::CRATE, &crate)) return false;

    // Voltage: 78.125 uV per LSB.
    const float voltage_v = ((float)vcell * 78.125f) / 1000000.0f;

    // SOC: 1/256 % per LSB.
    const float soc_percent = ((float)soc) / 256.0f;

    // CRATE: signed, 0.208 %/hour per LSB.
    const int16_t crate_signed = (int16_t)crate;
    const float crate_pct_per_hour = (float)crate_signed * 0.208f;

    out->ok = true;
    out->voltage_v = voltage_v;
    out->soc_percent = soc_percent;
    out->crate_percent_per_hour = crate_pct_per_hour;
    return true;
}
