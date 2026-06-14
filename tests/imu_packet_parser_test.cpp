// Standalone unit tests for ImuPacket parser.
// No BLE dependency — build and run this anywhere.
//
// Build via CMake:
//   cmake --build build --target imu_packet_parser_test
//   ./build/imu_packet_parser_test
//
// Or manually:
//   g++ -std=c++17 -I src tests/imu_packet_parser_test.cpp -o imu_packet_parser_test

#include "imu/ImuPacket.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

// ── Helper: make a valid 61-byte packet ──────────────────────────────────────

static ImuPacket sample_packet() {
    ImuPacket p{};
    p.seq   = 42;
    p.ax    = 0.12f;   p.ay = -0.34f;  p.az = 9.81f;
    p.gx    = 1.5f;    p.gy =  2.0f;   p.gz = -0.5f;
    p.roll  = 5.2f;    p.pitch = -3.1f; p.yaw = 123.4f;
    p.q0    = 0.999f;  p.q1 =  0.01f;  p.q2 = 0.0f;  p.q3 = 0.0f;
    p.esp_ms = 12345;
    return p;
}

// ── Test cases ────────────────────────────────────────────────────────────────

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, name) \
    do { \
        if (cond) { ++pass_count; std::printf("  PASS  %s\n", name); } \
        else      { ++fail_count; std::printf("  FAIL  %s  (line %d)\n", name, __LINE__); } \
    } while(0)

static void test_valid_parse() {
    std::printf("test_valid_parse\n");

    ImuPacket src = sample_packet();
    uint8_t raw[BLE_PACKET_LEN];
    build_imu_packet(src, raw);

    ImuPacket dst{};
    auto result = parse_imu_packet(raw, BLE_PACKET_LEN, dst);

    CHECK(result == ParseResult::OK, "returns OK");
    CHECK(dst.seq   == 42,           "seq");
    CHECK(std::abs(dst.ax    - 0.12f)   < 1e-5f, "ax");
    CHECK(std::abs(dst.ay    - (-0.34f)) < 1e-5f, "ay");
    CHECK(std::abs(dst.az    - 9.81f)   < 1e-4f, "az");
    CHECK(std::abs(dst.gx    - 1.5f)    < 1e-5f, "gx");
    CHECK(std::abs(dst.roll  - 5.2f)    < 1e-4f, "roll");
    CHECK(std::abs(dst.pitch - (-3.1f)) < 1e-4f, "pitch");
    CHECK(std::abs(dst.yaw   - 123.4f)  < 1e-3f, "yaw");
    CHECK(std::abs(dst.q0    - 0.999f)  < 1e-5f, "q0");
    CHECK(dst.esp_ms == 12345u,          "esp_ms");
}

static void test_wrong_length() {
    std::printf("test_wrong_length\n");

    ImuPacket src = sample_packet();
    uint8_t raw[BLE_PACKET_LEN];
    build_imu_packet(src, raw);

    ImuPacket dst{};

    // One byte short
    auto r1 = parse_imu_packet(raw, BLE_PACKET_LEN - 1, dst);
    CHECK(r1 == ParseResult::INVALID_LENGTH, "60 bytes -> INVALID_LENGTH");

    // One byte long
    uint8_t raw_long[BLE_PACKET_LEN + 1]{};
    std::memcpy(raw_long, raw, BLE_PACKET_LEN);
    auto r2 = parse_imu_packet(raw_long, BLE_PACKET_LEN + 1, dst);
    CHECK(r2 == ParseResult::INVALID_LENGTH, "62 bytes -> INVALID_LENGTH");

    // Zero bytes
    auto r3 = parse_imu_packet(raw, 0, dst);
    CHECK(r3 == ParseResult::INVALID_LENGTH, "0 bytes -> INVALID_LENGTH");
}

static void test_bad_magic() {
    std::printf("test_bad_magic\n");

    ImuPacket src = sample_packet();
    uint8_t raw[BLE_PACKET_LEN];
    build_imu_packet(src, raw);

    ImuPacket dst{};

    // Corrupt first magic byte
    uint8_t bad_magic[BLE_PACKET_LEN];
    std::memcpy(bad_magic, raw, BLE_PACKET_LEN);
    bad_magic[0] = 0x00;
    auto r1 = parse_imu_packet(bad_magic, BLE_PACKET_LEN, dst);
    CHECK(r1 == ParseResult::INVALID_MAGIC, "bad magic[0] -> INVALID_MAGIC");

    // Corrupt second magic byte
    std::memcpy(bad_magic, raw, BLE_PACKET_LEN);
    bad_magic[1] = 0x00;
    auto r2 = parse_imu_packet(bad_magic, BLE_PACKET_LEN, dst);
    CHECK(r2 == ParseResult::INVALID_MAGIC, "bad magic[1] -> INVALID_MAGIC");
}

static void test_checksum_error() {
    std::printf("test_checksum_error\n");

    ImuPacket src = sample_packet();
    uint8_t raw[BLE_PACKET_LEN];
    build_imu_packet(src, raw);

    ImuPacket dst{};

    // Flip a bit in a data byte
    uint8_t bad_csum[BLE_PACKET_LEN];
    std::memcpy(bad_csum, raw, BLE_PACKET_LEN);
    bad_csum[4] ^= 0x01;  // corrupt ax LSB
    auto r = parse_imu_packet(bad_csum, BLE_PACKET_LEN, dst);
    CHECK(r == ParseResult::CHECKSUM_ERROR, "corrupted payload -> CHECKSUM_ERROR");

    // Correct data but wrong checksum byte
    std::memcpy(bad_csum, raw, BLE_PACKET_LEN);
    bad_csum[60] ^= 0xFF;
    auto r2 = parse_imu_packet(bad_csum, BLE_PACKET_LEN, dst);
    CHECK(r2 == ParseResult::CHECKSUM_ERROR, "wrong checksum byte -> CHECKSUM_ERROR");
}

static void test_zero_quaternion_accepted() {
    std::printf("test_zero_quaternion_accepted\n");

    // Firmware may send q=0 when quaternion frame not received yet
    ImuPacket src = sample_packet();
    src.q0 = src.q1 = src.q2 = src.q3 = 0.0f;
    uint8_t raw[BLE_PACKET_LEN];
    build_imu_packet(src, raw);

    ImuPacket dst{};
    auto r = parse_imu_packet(raw, BLE_PACKET_LEN, dst);
    CHECK(r == ParseResult::OK, "zero quaternion is valid");
    CHECK(dst.q0 == 0.0f, "q0 == 0");
}

static void test_checksum_coverage() {
    std::printf("test_checksum_coverage\n");

    // Every byte [0..59] must contribute to checksum — flipping each should fail.
    ImuPacket src = sample_packet();
    uint8_t raw[BLE_PACKET_LEN];
    build_imu_packet(src, raw);

    int detected = 0;
    ImuPacket dst{};
    for (size_t i = 0; i < 60; ++i) {
        uint8_t bad[BLE_PACKET_LEN];
        std::memcpy(bad, raw, BLE_PACKET_LEN);
        bad[i] ^= 0xFF;
        auto r = parse_imu_packet(bad, BLE_PACKET_LEN, dst);
        // Bytes 0-1 are magic — flipping them triggers INVALID_MAGIC before checksum.
        if (i < 2) {
            if (r == ParseResult::INVALID_MAGIC) ++detected;
        } else {
            if (r == ParseResult::CHECKSUM_ERROR) ++detected;
        }
    }
    CHECK(detected == 60, "all 60 byte corruptions detected");
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {
    std::printf("=== ImuPacket parser tests ===\n\n");

    test_valid_parse();
    std::printf("\n");
    test_wrong_length();
    std::printf("\n");
    test_bad_magic();
    std::printf("\n");
    test_checksum_error();
    std::printf("\n");
    test_zero_quaternion_accepted();
    std::printf("\n");
    test_checksum_coverage();
    std::printf("\n");

    std::printf("=== %d passed, %d failed ===\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
