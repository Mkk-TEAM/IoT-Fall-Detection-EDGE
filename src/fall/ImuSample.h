#pragma once
#include <cstdint>
#include <cmath>
#include "../imu/ImuPacket.h"

/// Enriched IMU sample: raw BLE fields + pre-computed features.
struct ImuSample {
    // ── from BLE packet ───────────────────────────────────────────────────────
    uint16_t seq    = 0;
    uint32_t esp_ms = 0;
    int64_t  gw_ms  = 0;   // gateway epoch milliseconds
    float ax = 0, ay = 0, az = 0;   // m/s² or g (see accel_unit_mps2 in FallConfig)
    float gx = 0, gy = 0, gz = 0;   // deg/s
    float roll = 0, pitch = 0, yaw = 0;   // degrees
    float q0 = 1, q1 = 0, q2 = 0, q3 = 0;

    // ── pre-computed features (always in g / deg/s) ───────────────────────────
    float accel_mag_g     = 0;   // sqrt(ax²+ay²+az²) normalised to g
    float dynamic_accel_g = 0;   // |accel_mag_g - 1.0|
    float gyro_mag_dps    = 0;   // sqrt(gx²+gy²+gz²)

    // ── detector annotation (filled during push_sample) ───────────────────────
    int detector_state = 0;   // cast of FallState at moment of receipt
};

/// Build an ImuSample from a raw BLE packet.
/// If accel_is_mps2 == true, ax/ay/az are in m/s² and are divided by G to get g.
/// If false, they are already in g.
inline ImuSample make_sample(const ImuPacket &p, int64_t gw_ms, bool accel_is_mps2)
{
    constexpr float G = 9.80665f;
    ImuSample s;
    s.seq    = p.seq;
    s.esp_ms = p.esp_ms;
    s.gw_ms  = gw_ms;
    s.ax     = p.ax;  s.ay = p.ay;  s.az = p.az;
    s.gx     = p.gx;  s.gy = p.gy;  s.gz = p.gz;
    s.roll   = p.roll; s.pitch = p.pitch; s.yaw = p.yaw;
    s.q0     = p.q0;  s.q1 = p.q1;  s.q2 = p.q2; s.q3 = p.q3;

    float amag        = std::sqrt(s.ax*s.ax + s.ay*s.ay + s.az*s.az);
    s.accel_mag_g     = accel_is_mps2 ? amag / G : amag;
    s.dynamic_accel_g = std::fabs(s.accel_mag_g - 1.0f);
    s.gyro_mag_dps    = std::sqrt(s.gx*s.gx + s.gy*s.gy + s.gz*s.gz);
    return s;
}
