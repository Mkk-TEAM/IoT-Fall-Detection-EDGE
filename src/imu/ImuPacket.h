#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>

// 61-byte BLE packet from ESP32 FallDetect-IMU firmware (little-endian throughout).
//
// Offset  Len  Field
//  0-1     2   magic = 0xAA 0x55
//  2-3     2   seq        uint16
//  4-15   12   ax,ay,az   float × 3  (m/s²)
// 16-27   12   gx,gy,gz   float × 3  (deg/s)
// 28-39   12   roll,pitch,yaw  float × 3  (deg)
// 40-55   16   q0,q1,q2,q3    float × 4
// 56-59    4   esp_ms     uint32 (ms since ESP32 boot)
//   60     1   checksum = XOR of bytes [0..59]

static constexpr size_t  BLE_PACKET_LEN = 61;
static constexpr uint8_t BLE_MAGIC_0    = 0xAA;
static constexpr uint8_t BLE_MAGIC_1    = 0x55;

struct ImuPacket {
    uint16_t seq;
    float    ax, ay, az;
    float    gx, gy, gz;
    float    roll, pitch, yaw;
    float    q0, q1, q2, q3;
    uint32_t esp_ms;
};

enum class ParseResult {
    OK,
    INVALID_LENGTH,
    INVALID_MAGIC,
    CHECKSUM_ERROR,
};

inline const char *parse_result_str(ParseResult r) {
    switch (r) {
        case ParseResult::OK:             return "OK";
        case ParseResult::INVALID_LENGTH: return "INVALID_LENGTH";
        case ParseResult::INVALID_MAGIC:  return "INVALID_MAGIC";
        case ParseResult::CHECKSUM_ERROR: return "CHECKSUM_ERROR";
    }
    return "UNKNOWN";
}

inline ParseResult parse_imu_packet(const uint8_t *data, size_t len, ImuPacket &out) {
    if (len != BLE_PACKET_LEN) return ParseResult::INVALID_LENGTH;
    if (data[0] != BLE_MAGIC_0 || data[1] != BLE_MAGIC_1) return ParseResult::INVALID_MAGIC;

    uint8_t csum = 0;
    for (size_t i = 0; i < 60; ++i) csum ^= data[i];
    if (csum != data[60]) return ParseResult::CHECKSUM_ERROR;

    auto r_u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, data + o, 2); return v; };
    auto r_f32 = [&](size_t o) { float    v; std::memcpy(&v, data + o, 4); return v; };
    auto r_u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, data + o, 4); return v; };

    out.seq   = r_u16(2);
    out.ax    = r_f32(4);
    out.ay    = r_f32(8);
    out.az    = r_f32(12);
    out.gx    = r_f32(16);
    out.gy    = r_f32(20);
    out.gz    = r_f32(24);
    out.roll  = r_f32(28);
    out.pitch = r_f32(32);
    out.yaw   = r_f32(36);
    out.q0    = r_f32(40);
    out.q1    = r_f32(44);
    out.q2    = r_f32(48);
    out.q3    = r_f32(52);
    out.esp_ms = r_u32(56);

    return ParseResult::OK;
}

// Build a valid 61-byte packet from an ImuPacket struct (for tests / simulation).
inline void build_imu_packet(const ImuPacket &in, uint8_t out[BLE_PACKET_LEN]) {
    std::memset(out, 0, BLE_PACKET_LEN);
    out[0] = BLE_MAGIC_0;
    out[1] = BLE_MAGIC_1;

    auto w_u16 = [&](size_t o, uint16_t v) { std::memcpy(out + o, &v, 2); };
    auto w_f32 = [&](size_t o, float    v) { std::memcpy(out + o, &v, 4); };
    auto w_u32 = [&](size_t o, uint32_t v) { std::memcpy(out + o, &v, 4); };

    w_u16(2,  in.seq);
    w_f32(4,  in.ax);  w_f32(8,  in.ay);  w_f32(12, in.az);
    w_f32(16, in.gx);  w_f32(20, in.gy);  w_f32(24, in.gz);
    w_f32(28, in.roll); w_f32(32, in.pitch); w_f32(36, in.yaw);
    w_f32(40, in.q0);  w_f32(44, in.q1);  w_f32(48, in.q2);  w_f32(52, in.q3);
    w_u32(56, in.esp_ms);

    uint8_t csum = 0;
    for (size_t i = 0; i < 60; ++i) csum ^= out[i];
    out[60] = csum;
}
