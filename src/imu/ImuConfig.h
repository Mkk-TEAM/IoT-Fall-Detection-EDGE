#pragma once
#include <string>

struct ImuConfig {
    // ── BLE ──────────────────────────────────────────────────────────────────
    std::string ble_device_name      = "FallDetect-IMU";
    std::string ble_notify_char_uuid = "12345678-1234-1234-1234-123456789abc";
    int         ble_scan_timeout_ms  = 10000;  // per scan attempt

    // Reconnect backoff: starts at reconnect_initial_ms, doubles each retry,
    // caps at reconnect_max_ms.
    int reconnect_initial_ms = 1000;
    int reconnect_max_ms     = 10000;

    // ── Backend ───────────────────────────────────────────────────────────────
    std::string be_base_url  = "http://localhost:3000/api/v1";
    std::string edge_secret  = "";
    std::string device_id    = "imu_001";
    std::string gateway_id   = "gw_001";

    // How often to POST health-logs to backend (seconds).
    // Should match HEALTH_LOG_INTERVAL_SECONDS in the simulator .env.
    int post_interval_sec = 5;

    // ── Clip / video URLs (forwarded in events, not used by BLE client itself)
    std::string stream_host = "127.0.0.1";
    int         stream_port = 8081;
    std::string clip_dir    = "/media/usb/camera";

    // ── Logging ───────────────────────────────────────────────────────────────
    int stats_interval_sec = 10;  // log packet stats every N seconds

    // ── IMU health HTTP server ────────────────────────────────────────────────
    std::string health_host = "0.0.0.0";
    int         health_port = 8082;

    // ── Fall detection ────────────────────────────────────────────────────────
    // Path to fall_detector_config.json (threshold config, no rebuild needed).
    std::string fall_config_file = "fall_detector_config.json";
};
