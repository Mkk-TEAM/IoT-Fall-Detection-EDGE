#include "EventPoster.h"
#include <httplib.h>
#include <cstdio>
#include <iostream>

// Split "http://host:port/api/v1" → {"http://host:port", "/api/v1"}
static std::pair<std::string, std::string> split_url(const std::string &url) {
    auto se = url.find("://");
    if (se == std::string::npos) return {url, ""};
    auto ps = url.find('/', se + 3);
    if (ps == std::string::npos) return {url, ""};
    return {url.substr(0, ps), url.substr(ps)};
}

EventPoster::EventPoster(const ImuConfig &cfg) : cfg_(cfg) {
    auto [host, prefix] = split_url(cfg_.be_base_url);
    be_host_    = host;
    api_prefix_ = prefix;
}

int EventPoster::post(const FallEvent &ev, const std::string &local_dir) {
    if (ev.event_type != "fall_confirmed") return 0;

    std::string body = build_json(ev, local_dir);
    std::string path = api_prefix_ + "/internal/events";

    httplib::Client cli(be_host_);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

    auto res = cli.Post(path,
        httplib::Headers{
            {"Content-Type",  "application/json"},
            {"X-Edge-Secret", cfg_.edge_secret},
        },
        body, "application/json");

    if (!res) {
        std::cerr << "[EventPoster] Connection error → " << be_host_ << path << '\n';
        return -1;
    }
    std::printf("[EventPoster] POST %s → HTTP %d\n", path.c_str(), res->status);
    if (res->status < 200 || res->status >= 300)
        std::cerr << "[EventPoster] Body: " << res->body.substr(0, 200) << '\n';
    return res->status;
}

std::string EventPoster::build_json(const FallEvent &ev,
                                     const std::string &local_dir) const {
    std::string reasons_json = "[";
    for (size_t i = 0; i < ev.reasons.size(); ++i) {
        if (i) reasons_json += ',';
        reasons_json += '"'; reasons_json += ev.reasons[i]; reasons_json += '"';
    }
    reasons_json += ']';

    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "{"
        "\"eventType\":\"FALL\","
        "\"source\":\"imu\","
        "\"priority\":\"CRITICAL\","
        "\"gatewayId\":\"%s\","
        "\"deviceId\":\"%s\","
        "\"timestamp\":\"%s\","
        "\"confidence\":%.2f,"
        "\"reason\":%s,"
        "\"message\":\"Fall detected by IMU edge gateway\","
        "\"metrics\":{"
          "\"peakAccelG\":%.2f,"
          "\"minAccelG\":%.2f,"
          "\"peakGyroDps\":%.1f,"
          "\"tiltChangeDeg\":%.1f,"
          "\"inactivityDurationMs\":%lld"
        "},"
        "\"localImuWindowFile\":\"%s/imu_window.csv\""
        "}",
        cfg_.gateway_id.c_str(),
        cfg_.device_id.c_str(),
        ev.timestamp.c_str(),
        ev.confidence,
        reasons_json.c_str(),
        ev.peak_accel_g, ev.min_accel_g,
        ev.peak_gyro_dps, ev.tilt_change_deg,
        (long long)ev.inactivity_duration_ms,
        local_dir.c_str());
    return buf;
}
