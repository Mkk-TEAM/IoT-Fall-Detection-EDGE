#include "BleImuClient.h"

#include <simpleble/SimpleBLE.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

// ── Logging helpers ───────────────────────────────────────────────────────────

static void ble_log(const char *tag, const std::string &msg) {
    std::cerr << "[" << tag << "] " << msg << '\n';
}

// ── Public API ────────────────────────────────────────────────────────────────

BleImuClient::BleImuClient(ImuConfig config)
    : config_(std::move(config))
    , hz_window_start_(std::chrono::steady_clock::now()) {}

BleImuClient::~BleImuClient() { stop(); }

void BleImuClient::set_on_packet(PacketCallback cb) {
    on_packet_ = std::move(cb);
}

void BleImuClient::stop() {
    running_ = false;
}

bool BleImuClient::is_connected() const {
    auto s = state_.load();
    return s == State::RECEIVING || s == State::SUBSCRIBING;
}

const char *BleImuClient::state_str(State s) {
    switch (s) {
        case State::IDLE:         return "IDLE";
        case State::SCANNING:     return "SCANNING";
        case State::CONNECTING:   return "CONNECTING";
        case State::SUBSCRIBING:  return "SUBSCRIBING";
        case State::RECEIVING:    return "RECEIVING";
        case State::DISCONNECTED: return "DISCONNECTED";
    }
    return "UNKNOWN";
}

BleImuClient::Stats BleImuClient::get_stats() const {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    Stats s = stats_;
    if (s.packets_valid > 0) {
        s.last_packet_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_packet_tp_).count();
    }
    return s;
}

// ── Notification handler (called on SimpleBLE's internal thread) ──────────────

void BleImuClient::handle_notification(const uint8_t *data, size_t len) {
    ImuPacket pkt;
    auto result = parse_imu_packet(data, len, pkt);

    std::lock_guard<std::mutex> lk(stats_mutex_);
    ++stats_.packets_received;

    switch (result) {
        case ParseResult::INVALID_LENGTH:
            ++stats_.invalid_length;
            std::cerr << "[BLE] reject: bad length " << len << " (need 61)\n";
            return;
        case ParseResult::INVALID_MAGIC:
            ++stats_.invalid_magic;
            std::cerr << "[BLE] reject: bad magic 0x"
                      << std::hex << (int)data[0] << (int)data[1] << std::dec << '\n';
            return;
        case ParseResult::CHECKSUM_ERROR:
            ++stats_.checksum_errors;
            std::cerr << "[BLE] reject: checksum mismatch\n";
            return;
        case ParseResult::OK:
            break;
    }

    ++stats_.packets_valid;
    stats_.last_seq = pkt.seq;
    last_packet_tp_ = std::chrono::steady_clock::now();

    // Hz estimate — update over 5-second window
    ++hz_window_count_;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        last_packet_tp_ - hz_window_start_).count();
    if (elapsed_ms >= 5000) {
        stats_.estimated_hz = hz_window_count_ * 1000.0 / elapsed_ms;
        hz_window_count_    = 0;
        hz_window_start_    = last_packet_tp_;
    }

    if (on_packet_) {
        // on_packet_ runs on SimpleBLE's thread.
        // BackendPoster::update() is non-blocking (mutex + copy), safe here.
        on_packet_(pkt);
    }
}

// ── Stats logging ─────────────────────────────────────────────────────────────

void BleImuClient::log_stats() const {
    auto s = get_stats();
    std::cerr << "[BLE] stats:"
              << " recv=" << s.packets_received
              << " valid=" << s.packets_valid
              << " bad_len=" << s.invalid_length
              << " bad_magic=" << s.invalid_magic
              << " csum_err=" << s.checksum_errors
              << " reconnects=" << s.reconnect_count
              << " last_seq=" << s.last_seq
              << " hz=" << s.estimated_hz
              << " age_ms=" << s.last_packet_age_ms
              << '\n';
}

// ── One scan→connect→subscribe→receive attempt ────────────────────────────────

void BleImuClient::scan_and_connect_once() {
    if (!SimpleBLE::Adapter::bluetooth_enabled()) {
        throw std::runtime_error("Bluetooth adapter not available / not enabled");
    }

    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        throw std::runtime_error("No Bluetooth adapters found");
    }

    auto &adapter = adapters[0];
    ble_log("BLE", "adapter: " + adapter.identifier() + " [" + adapter.address() + "]");

    // ── Phase 1: Scan ─────────────────────────────────────────────────────────
    state_ = State::SCANNING;
    ble_log("BLE", "State: SCANNING for \"" + config_.ble_device_name + "\"");

    std::mutex          found_mutex;
    std::optional<SimpleBLE::Peripheral> found;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        if (p.identifier() == config_.ble_device_name) {
            std::lock_guard<std::mutex> lk(found_mutex);
            if (!found) {
                found = p;
                adapter.scan_stop();
            }
        }
    });

    adapter.scan_start();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(config_.ble_scan_timeout_ms);
    while (running_) {
        std::this_thread::sleep_for(100ms);
        std::lock_guard<std::mutex> lk(found_mutex);
        if (found || std::chrono::steady_clock::now() >= deadline) break;
    }

    if (adapter.scan_is_active()) adapter.scan_stop();

    SimpleBLE::Peripheral device;
    {
        std::lock_guard<std::mutex> lk(found_mutex);
        if (!found) {
            ble_log("BLE", "Device \"" + config_.ble_device_name + "\" not found — will retry");
            return;
        }
        device = *found;
    }

    ble_log("BLE", "Found: " + device.identifier() + " [" + device.address() + "]");

    // ── Phase 2: Connect ──────────────────────────────────────────────────────
    state_ = State::CONNECTING;
    ble_log("BLE", "State: CONNECTING");

    device.connect();

    if (!device.is_connected()) {
        ble_log("BLE", "Connection failed — will retry");
        return;
    }
    ble_log("BLE", "Connected");

    // ── Phase 3: Subscribe ────────────────────────────────────────────────────
    state_ = State::SUBSCRIBING;
    ble_log("BLE", "State: SUBSCRIBING");

    std::atomic<bool> device_alive{true};
    device.set_callback_on_disconnected([&]() {
        device_alive = false;
        ble_log("BLE", "Device disconnected");
    });

    bool subscribed = false;
    for (auto &svc : device.services()) {
        for (auto &chr : svc.characteristics()) {
            if (chr.uuid() == config_.ble_notify_char_uuid) {
                device.notify(svc.uuid(), chr.uuid(),
                    [this](SimpleBLE::ByteArray bytes) {
                        handle_notification(
                            reinterpret_cast<const uint8_t *>(bytes.data()),
                            bytes.size());
                    });
                subscribed = true;
                ble_log("BLE", "Subscribed to char " + chr.uuid() +
                               " on service " + svc.uuid());
                break;
            }
        }
        if (subscribed) break;
    }

    if (!subscribed) {
        ble_log("BLE", "Notify characteristic " + config_.ble_notify_char_uuid + " not found");
        device.disconnect();
        return;
    }

    // ── Phase 4: Receive loop ─────────────────────────────────────────────────
    state_ = State::RECEIVING;
    ble_log("BLE", "State: RECEIVING");

    auto last_stats_tp = std::chrono::steady_clock::now();

    while (running_ && device_alive) {
        std::this_thread::sleep_for(500ms);

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_stats_tp).count() >= config_.stats_interval_sec) {
            log_stats();
            last_stats_tp = now;
        }
    }

    if (device.is_connected()) device.disconnect();
}

// ── Main reconnect loop ───────────────────────────────────────────────────────

void BleImuClient::run() {
    running_ = true;
    hz_window_start_ = std::chrono::steady_clock::now();

    int backoff_ms = config_.reconnect_initial_ms;

    while (running_) {
        try {
            scan_and_connect_once();
        } catch (const std::exception &e) {
            ble_log("BLE", std::string("Exception: ") + e.what());
        } catch (...) {
            ble_log("BLE", "Unknown exception in BLE loop");
        }

        if (!running_) break;

        state_ = State::DISCONNECTED;
        {
            std::lock_guard<std::mutex> lk(stats_mutex_);
            ++stats_.reconnect_count;
        }

        ble_log("BLE", "State: DISCONNECTED — retrying in " +
                        std::to_string(backoff_ms) + "ms");

        // Sleep with running_ check every 200ms
        for (int i = 0; i < backoff_ms / 200 && running_; ++i) {
            std::this_thread::sleep_for(200ms);
        }

        // Reset backoff if we've received packets (connection was working)
        {
            std::lock_guard<std::mutex> lk(stats_mutex_);
            if (stats_.packets_valid > 0) {
                backoff_ms = config_.reconnect_initial_ms;
            }
        }

        backoff_ms = std::min(backoff_ms * 2, config_.reconnect_max_ms);
    }

    state_ = State::IDLE;
    ble_log("BLE", "Stopped");
}
