#include "BackendPoster.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

// ── URL helpers ───────────────────────────────────────────────────────────────

// Split "http://host:3000/api/v1" into {"http://host:3000", "/api/v1"}.
static std::pair<std::string, std::string> split_base_url(const std::string &url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {url, ""};
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) return {url, ""};
    return {url.substr(0, path_start), url.substr(path_start)};
}

// ── Construction ─────────────────────────────────────────────────────────────

BackendPoster::BackendPoster(ImuConfig config) : config_(std::move(config)) {
    auto [host, prefix] = split_base_url(config_.be_base_url);
    be_host_    = host;    // "http://localhost:3000"
    api_prefix_ = prefix;  // "/api/v1"
}

BackendPoster::~BackendPoster() { stop(); }

// ── Lifecycle ────────────────────────────────────────────────────────────────

void BackendPoster::start() {
    running_ = true;
    thread_  = std::thread(&BackendPoster::thread_func, this);
}

void BackendPoster::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// ── Public update (non-blocking, called from BLE notification thread) ─────────

void BackendPoster::update(const ImuPacket &pkt) {
    std::lock_guard<std::mutex> lk(pkt_mutex_);
    latest_pkt_ = pkt;
    has_pkt_    = true;
}

// ── Stats ────────────────────────────────────────────────────────────────────

BackendPoster::Stats BackendPoster::get_stats() const {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    return stats_;
}

// ── Background posting thread ────────────────────────────────────────────────

void BackendPoster::thread_func() {
    // Sleep in 200ms chunks so stop() isn't delayed by the full interval.
    const int chunks = (config_.post_interval_sec * 1000) / 200;

    while (running_) {
        for (int i = 0; i < chunks && running_; ++i) {
            std::this_thread::sleep_for(200ms);
        }
        if (!running_) break;

        ImuPacket pkt;
        bool has;
        {
            std::lock_guard<std::mutex> lk(pkt_mutex_);
            has = has_pkt_;
            if (has) pkt = latest_pkt_;
        }

        if (!has) continue;

        auto body = build_health_log_json(pkt, config_.device_id, config_.gateway_id);

        {
            std::lock_guard<std::mutex> lk(stats_mutex_);
            ++stats_.posts_attempted;
        }

        bool ok = do_post("/internal/device-status-logs", body);

        {
            std::lock_guard<std::mutex> lk(stats_mutex_);
            if (ok) ++stats_.posts_ok;
            else    ++stats_.posts_failed;
        }
    }
}

// ── HTTP POST ────────────────────────────────────────────────────────────────

bool BackendPoster::do_post(const std::string &path, const std::string &body) {
    httplib::Client cli(be_host_);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(5);

    httplib::Headers headers = {
        {"Content-Type",  "application/json"},
        {"X-Edge-Secret", config_.edge_secret},
    };

    std::string full_path = api_prefix_ + path;
    auto res = cli.Post(full_path, headers, body, "application/json");

    if (!res) {
        std::cerr << "[POST] Connection error: " << be_host_ << full_path << '\n';
        return false;
    }
    if (res->status < 200 || res->status >= 300) {
        std::cerr << "[POST] HTTP " << res->status << " " << full_path
                  << " | " << res->body.substr(0, 120) << '\n';
        return false;
    }
    return true;
}

// ── JSON builder ─────────────────────────────────────────────────────────────

std::string BackendPoster::utc_now_iso8601() {
    auto now  = std::chrono::system_clock::now();
    auto tt   = std::chrono::system_clock::to_time_t(now);
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()).count() % 1000;
    struct tm t{};
    gmtime_r(&tt, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec, ms);
    return buf;
}

std::string BackendPoster::build_health_log_json(const ImuPacket &p,
                                                  const std::string &dev,
                                                  const std::string &gw) {
    const float tilt = std::abs(p.pitch);
    const float acc_mag  = std::sqrt(p.ax*p.ax + p.ay*p.ay + p.az*p.az);
    const float movement = std::max(0.0f, std::min(1.0f, std::abs(acc_mag - 9.81f) / 9.81f));

    // Payload matches POST /internal/device-status-logs schema:
    // { deviceId, gatewayId, status, source, timestamp, rawPayload: { imu data } }
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "{"
        "\"deviceId\":\"%s\","
        "\"gatewayId\":\"%s\","
        "\"status\":\"ONLINE\","
        "\"source\":\"ble\","
        "\"timestamp\":\"%s\","
        "\"rawPayload\":{"
          "\"seq\":%u,"
          "\"accelX\":%.4f,\"accelY\":%.4f,\"accelZ\":%.4f,"
          "\"gyroX\":%.4f,\"gyroY\":%.4f,\"gyroZ\":%.4f,"
          "\"roll\":%.4f,\"pitch\":%.4f,\"yaw\":%.4f,"
          "\"q0\":%.6f,\"q1\":%.6f,\"q2\":%.6f,\"q3\":%.6f,"
          "\"tiltAngle\":%.4f,"
          "\"movementLevel\":%.4f,"
          "\"espMs\":%u"
        "}"
        "}",
        dev.c_str(), gw.c_str(),
        utc_now_iso8601().c_str(),
        (unsigned)p.seq,
        p.ax, p.ay, p.az,
        p.gx, p.gy, p.gz,
        p.roll, p.pitch, p.yaw,
        p.q0, p.q1, p.q2, p.q3,
        tilt, movement,
        (unsigned)p.esp_ms);
    return buf;
}
