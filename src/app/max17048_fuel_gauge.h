#ifndef MAX17048_FUEL_GAUGE_H
#define MAX17048_FUEL_GAUGE_H

#include <Arduino.h>

// Minimal MAX17048 (ModelGauge) helper for FeatherS3[D].
// Reads: voltage (VCELL), state-of-charge (SOC), and SOC change rate (CRATE).
//
// Notes:
// - I2C address: 0x36
// - Register reads are 16-bit big-endian.
// - Scaling matches MAX17048 datasheet and UMSeriesD helper:
//   - Voltage: 78.125 uV/LSB
//   - SOC: 1/256 % per LSB
//   - CRATE: 0.208 % per hour per LSB (signed)

struct Max17048Reading {
    bool ok;
    float voltage_v;
    float soc_percent;
    float crate_percent_per_hour;
};

// Initialize the fuel gauge if present.
// Safe to call multiple times.
void max17048_init();

// Returns true if the gauge was detected during init.
bool max17048_available();

// Read voltage/SOC/CRATE. Returns false on I2C failure or if not available.
bool max17048_read(Max17048Reading* out);

#endif // MAX17048_FUEL_GAUGE_H
