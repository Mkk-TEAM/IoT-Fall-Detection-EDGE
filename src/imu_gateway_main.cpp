#include "imu/BleImuClient.h"
#include "imu/ImuConfig.h"
#include "fall/FallConfig.h"
#include "fall/FallDetector.h"
#include "fall/EventStore.h"
#include "fall/EventPoster.h"
#include "fall/ImuSample.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// ── .env loader ───────────────────────────────────────────────────────────────

namespace {

std::atomic<bool> g_running{true};
httplib::Server  *g_health_server = nullptr;

static std::string trim(const std::string &s) {
    auto f = s.find_first_not_of(" \t\r\n");
    if (f == std::string::npos) return {};
    return s.substr(f, s.find_last_not_of(" \t\r\n") - f + 1);
}

static void load_dotenv(const std::string &path) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto sep = line.find('=');
        if (sep == std::string::npos) continue;
        auto k = trim(line.substr(0, sep));
        auto v = trim(line.substr(sep + 1));
        if (v.size() >= 2 &&
            ((v.front() == '"' && v.back() == '"') ||
             (v.front() == '\'' && v.back() == '\'')))
            v = v.substr(1, v.size() - 2);
        if (!k.empty() && !std::getenv(k.c_str()))
            setenv(k.c_str(), v.c_str(), 0);
    }
}

static std::string env_str(const char *key, const std::string &def) {
    const auto *v = std::getenv(key);
    return v ? v : def;
}
static int env_int(const char *key, int def) {
    const auto *v = std::getenv(key);
    if (!v) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

static ImuConfig load_imu_config() {
    load_dotenv(".env");
    ImuConfig c;
    c.ble_device_name      = env_str("BLE_DEVICE_NAME",        c.ble_device_name);
    c.ble_notify_char_uuid = env_str("BLE_NOTIFY_CHAR_UUID",   c.ble_notify_char_uuid);
    c.ble_scan_timeout_ms  = env_int("BLE_SCAN_TIMEOUT_MS",    c.ble_scan_timeout_ms);
    c.reconnect_initial_ms = env_int("BLE_RECONNECT_INIT_MS",  c.reconnect_initial_ms);
    c.reconnect_max_ms     = env_int("BLE_RECONNECT_MAX_MS",   c.reconnect_max_ms);
    c.be_base_url          = env_str("BE_BASE_URL",            c.be_base_url);
    c.edge_secret          = env_str("EDGE_SECRET",            c.edge_secret);
    c.device_id            = env_str("IMU_DEVICE_ID",          c.device_id);
    c.gateway_id           = env_str("GATEWAY_ID",             c.gateway_id);
    c.stream_host          = env_str("EDGE_PUBLIC_HOST",       c.stream_host);
    c.stream_port          = env_int("EDGE_STREAM_PORT",       c.stream_port);
    c.clip_dir             = env_str("CLIP_DIR",               c.clip_dir);
    c.stats_interval_sec   = env_int("IMU_STATS_INTERVAL_SEC", c.stats_interval_sec);
    c.health_host          = env_str("IMU_HEALTH_HOST",        c.health_host);
    c.health_port          = env_int("IMU_HEALTH_PORT",        c.health_port);
    c.fall_config_file     = env_str("FALL_DETECTOR_CONFIG",   c.fall_config_file);
    return c;
}

static void signal_handler(int) {
    g_running = false;
    if (g_health_server) g_health_server->stop();
}

static int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                "Usage: edge-imu-ble [--config <fall_detector_config.json>]\n"
                "  Real-time BLE fall detection gateway.\n"
                "  Thresholds: edit fall_detector_config.json (no rebuild needed).\n");
            return 0;
        }
    }

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Load configs ──────────────────────────────────────────────────────────
    ImuConfig  imu_cfg  = load_imu_config();
    FallConfig fall_cfg = FallConfig::load(imu_cfg.fall_config_file);

    std::fprintf(stderr, "[IMU] BLE fall detection gateway starting\n"
                         "[IMU]   device  : %s\n"
                         "[IMU]   backend : %s\n"
                         "[IMU]   deviceId: %s  gatewayId: %s\n"
                         "[IMU]   fall_config: %s\n",
                 imu_cfg.ble_device_name.c_str(),
                 imu_cfg.be_base_url.c_str(),
                 imu_cfg.device_id.c_str(),
                 imu_cfg.gateway_id.c_str(),
                 imu_cfg.fall_config_file.c_str());
    fall_cfg.print();

    // ── Fall detection pipeline ───────────────────────────────────────────────
    FallDetector detector(fall_cfg);
    EventStore   store(fall_cfg.event_data_dir);
    EventPoster  poster(imu_cfg);

    // Event queue: BLE notification thread enqueues; processor thread handles
    // file I/O and HTTP POST asynchronously (avoids blocking BLE callbacks).
    std::queue<FallEvent>     event_queue;
    std::mutex                event_queue_mu;
    std::condition_variable   event_queue_cv;
    std::atomic<bool>         event_proc_running{true};

    std::thread event_processor([&]() {
        while (event_proc_running || [&]{ std::lock_guard<std::mutex> lk(event_queue_mu); return !event_queue.empty(); }()) {
            std::unique_lock<std::mutex> lk(event_queue_mu);
            event_queue_cv.wait_for(lk, std::chrono::milliseconds(200));
            while (!event_queue.empty()) {
                FallEvent ev = std::move(event_queue.front());
                event_queue.pop();
                lk.unlock();

                // Compute expected dir path for the backend JSON payload.
                std::string expected_dir = fall_cfg.event_data_dir + "/" + ev.event_id;

                // POST confirmed events to backend first (so we have the result).
                std::string be_result;
                if (ev.event_type == "fall_confirmed") {
                    int code = poster.post(ev, expected_dir);
                    if (code >= 200 && code < 300)
                        be_result = "ok";
                    else if (code == -1)
                        be_result = "connection_error";
                    else
                        be_result = "http_" + std::to_string(code);
                }

                // Save event.json + imu_window.csv locally.
                store.save(ev, be_result);

                lk.lock();
            }
        }
    });

    detector.set_on_event([&](const FallEvent &ev) {
        {
            std::lock_guard<std::mutex> lk(event_queue_mu);
            event_queue.push(ev);
        }
        event_queue_cv.notify_one();
    });

    // ── BLE client ────────────────────────────────────────────────────────────
    BleImuClient ble(imu_cfg);
    ble.set_on_packet([&](const ImuPacket &pkt) {
        auto sample = make_sample(pkt, now_epoch_ms(), fall_cfg.accel_unit_mps2);
        detector.push_sample(sample);
    });

    // ── Timer thread: tick detector when BLE disconnects during POST_IMPACT ───
    std::thread ticker([&]() {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            detector.tick(now_epoch_ms());
        }
    });

    // ── Health HTTP server ────────────────────────────────────────────────────
    httplib::Server health_server;
    g_health_server = &health_server;

    health_server.Get("/health", [&](const httplib::Request &,
                                     httplib::Response &res) {
        auto cs  = ble.get_stats();
        auto ds  = detector.get_stats();
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "{"
            "\"imu_connected\":%s,"
            "\"ble_state\":\"%s\","
            "\"last_packet_age_ms\":%lld,"
            "\"packets_valid\":%llu,"
            "\"estimated_hz\":%.1f,"
            "\"reconnect_count\":%llu,"
            "\"seq_gaps\":%llu,"
            "\"loss_rate_pct\":%.2f,"
            "\"jitter_mean_ms\":%.2f,"
            "\"jitter_p95_ms\":%.2f,"
            "\"detector_state\":\"%s\","
            "\"samples_processed\":%llu,"
            "\"fall_candidates\":%llu,"
            "\"fall_confirmed\":%llu,"
            "\"fall_rejected\":%llu"
            "}",
            ble.is_connected() ? "true" : "false",
            BleImuClient::state_str(ble.get_state()),
            (long long)cs.last_packet_age_ms,
            (unsigned long long)cs.packets_valid,
            cs.estimated_hz,
            (unsigned long long)cs.reconnect_count,
            (unsigned long long)cs.seq_gaps,
            cs.loss_rate * 100.0,
            cs.jitter_mean_ms,
            cs.jitter_p95_ms,
            detector.get_state_str().c_str(),
            (unsigned long long)ds.samples,
            (unsigned long long)ds.candidates,
            (unsigned long long)ds.confirmed,
            (unsigned long long)ds.rejected);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-store");
        res.set_content(buf, "application/json");
    });

    std::thread health_thread([&]() {
        std::fprintf(stderr, "[IMU] Health: http://%s:%d/health\n",
                     imu_cfg.health_host.c_str(), imu_cfg.health_port);
        health_server.listen(imu_cfg.health_host, imu_cfg.health_port);
    });

    // ── IMU heartbeat → BE (PATCH /internal/devices/:id/heartbeat) ───────────
    // Posts BLE RTT metrics every 30s so BE shows imu_001 ONLINE with quality data.
    std::thread imu_heartbeat([&]() {
        const std::string &be_url = imu_cfg.be_base_url;  // e.g. http://localhost:3000/api/v1
        auto [hb_host, hb_prefix] = [&]() -> std::pair<std::string, std::string> {
            auto se = be_url.find("://");
            auto ps = be_url.find('/', se + 3);
            return { be_url.substr(0, ps), be_url.substr(ps) };
        }();
        std::string path = hb_prefix + "/internal/devices/" + imu_cfg.device_id + "/heartbeat";

        while (g_running) {
            // Spread first heartbeat 10s after start
            for (int i = 0; i < 100 && g_running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(300));

            auto cs = ble.get_stats();
            const char *status = ble.is_connected() ? "ONLINE" : "OFFLINE";

            char body[512];
            std::snprintf(body, sizeof(body),
                "{"
                "\"status\":\"%s\","
                "\"metrics\":{"
                  "\"estimatedHz\":%.2f,"
                  "\"jitterMeanMs\":%.2f,"
                  "\"jitterP95Ms\":%.2f,"
                  "\"lossRatePct\":%.2f,"
                  "\"seqGaps\":%llu,"
                  "\"reconnects\":%llu"
                "}"
                "}",
                status,
                cs.estimated_hz,
                cs.jitter_mean_ms,
                cs.jitter_p95_ms,
                cs.loss_rate * 100.0,
                (unsigned long long)cs.seq_gaps,
                (unsigned long long)cs.reconnect_count);

            try {
                httplib::Client cli(hb_host);
                cli.set_connection_timeout(3);
                cli.set_read_timeout(5);
                auto r = cli.Patch(path,
                    httplib::Headers{{"X-Edge-Secret", imu_cfg.edge_secret}},
                    body, "application/json");
                if (r)
                    std::fprintf(stderr, "[IMU] heartbeat → %s  Hz=%.1f  jitter=%.1fms  loss=%.2f%%\n",
                                 status, cs.estimated_hz, cs.jitter_mean_ms, cs.loss_rate * 100.0);
            } catch (...) {}
        }
    });

    // ── Shutdown watcher ──────────────────────────────────────────────────────
    std::thread watcher([&]() {
        while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ble.stop();
    });

    ble.run();  // blocks until stop()

    // ── Cleanup ───────────────────────────────────────────────────────────────
    if (health_server.is_running()) health_server.stop();

    event_proc_running = false;
    event_queue_cv.notify_all();
    event_processor.join();

    ticker.join();
    watcher.join();
    imu_heartbeat.join();
    health_thread.join();
    g_health_server = nullptr;

    std::fprintf(stderr, "[IMU] BLE fall detection gateway exited cleanly\n");
    return 0;
}
