#pragma once
#include "ImuConfig.h"
#include "ImuPacket.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

// BLE GATT client that scans for FallDetect-IMU, subscribes to the IMU
// notification characteristic, and fires on_packet() for every valid frame.
//
// Lifecycle:
//   BleImuClient client(config);
//   client.set_on_packet([](const ImuPacket& p) { ... });
//   client.run();   // blocks; auto-reconnects on disconnect
//   // call client.stop() from another thread (or signal handler) to exit
class BleImuClient {
public:
    enum class State {
        IDLE,
        SCANNING,
        CONNECTING,
        SUBSCRIBING,
        RECEIVING,
        DISCONNECTED,
    };

    struct Stats {
        uint64_t packets_received = 0;
        uint64_t packets_valid    = 0;
        uint64_t invalid_length   = 0;
        uint64_t invalid_magic    = 0;
        uint64_t checksum_errors  = 0;
        uint64_t reconnect_count  = 0;
        uint16_t last_seq         = 0;
        int64_t  last_packet_age_ms = -1;  // -1 = no packet received yet
        double   estimated_hz       = 0.0;
    };

    using PacketCallback = std::function<void(const ImuPacket &)>;

    explicit BleImuClient(ImuConfig config);
    ~BleImuClient();

    // Register callback invoked on every valid, parsed packet.
    // Called from SimpleBLE's internal thread — keep it fast (no blocking I/O).
    void set_on_packet(PacketCallback cb);

    // Block until stop() is called. Reconnects on disconnect with backoff.
    void run();

    // Signal run() to return. Safe to call from any thread.
    void stop();

    Stats get_stats() const;
    State get_state() const { return state_.load(); }
    bool  is_connected() const;

    static const char *state_str(State s);

private:
    // One attempt: scan → connect → subscribe → receive loop.
    // Returns when the device disconnects or running_ becomes false.
    void scan_and_connect_once();

    // Called from SimpleBLE's notification thread.
    void handle_notification(const uint8_t *data, size_t len);

    void log_stats() const;

    ImuConfig      config_;
    PacketCallback on_packet_;

    std::atomic<bool>  running_{false};
    std::atomic<State> state_{State::IDLE};

    mutable std::mutex stats_mutex_;
    Stats stats_;
    std::chrono::steady_clock::time_point last_packet_tp_;

    // Hz estimation over a sliding 5-second window
    std::chrono::steady_clock::time_point hz_window_start_;
    uint64_t hz_window_count_ = 0;
};
