#include "EventPoster.h"
#include <httplib.h>
#include <cstdio>
#include <ctime>
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

// Compute clip segment start time for a given unix timestamp.
// Recorder writes 5-min segments named by their start time: YYYY-MM-DD/HH-MM.
static void clip_time_str(time_t t, char *date_buf, size_t dsz,
                           char *hhmm_buf, size_t hsz) {
    // Floor to nearest 5-minute boundary
    t -= (t % 300);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    // Date in local timezone so it matches the recorder's file names
    // (recorder uses localtime via Python datetime.now())
    time_t local_t = t;
    struct tm tm_local;
    localtime_r(&local_t, &tm_local);
    std::strftime(date_buf, dsz, "%Y-%m-%d",  &tm_local);
    std::strftime(hhmm_buf, hsz, "%H-%M",     &tm_local);
}

std::string EventPoster::build_json(const FallEvent &ev,
                                     const std::string &local_dir) const {
    std::string reasons_json = "[";
    for (size_t i = 0; i < ev.reasons.size(); ++i) {
        if (i) reasons_json += ',';
        reasons_json += '"'; reasons_json += ev.reasons[i]; reasons_json += '"';
    }
    reasons_json += ']';

    // Build snapshot/video URLs from the camera recording service.
    // ev.timestamp is ISO-8601 UTC; parse it to get clip segment.
    char date_str[16] = {}, hhmm_str[8] = {};
    {
        struct tm tm {};
        if (std::sscanf(ev.timestamp.c_str(), "%d-%d-%dT%d:%d:%d",
                        &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                        &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900; tm.tm_mon -= 1;
            time_t t = timegm(&tm);  // UTC→unix
            clip_time_str(t, date_str, sizeof(date_str), hhmm_str, sizeof(hhmm_str));
        } else {
            // Fallback: use current time
            clip_time_str(std::time(nullptr), date_str, sizeof(date_str), hhmm_str, sizeof(hhmm_str));
        }
    }

    std::string base_url = "http://" + cfg_.stream_host + ":" +
                           std::to_string(cfg_.stream_port);
    std::string snapshot_url    = base_url + "/thumbs/" + date_str + "/" + hhmm_str + ".jpg";
    std::string video_url       = base_url + "/clips/"  + date_str + "/" + hhmm_str + ".mp4";

    char buf[2048];
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
        "\"snapshotUrl\":\"%s\","
        "\"relatedVideoUrl\":\"%s\","
        "\"metrics\":{"
          "\"peakAccelG\":%.2f,"
          "\"minAccelG\":%.2f,"
          "\"peakGyroDps\":%.1f,"
          "\"tiltChangeDeg\":%.1f"
        "},"
        "\"localImuWindowFile\":\"%s/imu_window.csv\""
        "}",
        cfg_.gateway_id.c_str(),
        cfg_.device_id.c_str(),
        ev.timestamp.c_str(),
        ev.confidence,
        reasons_json.c_str(),
        snapshot_url.c_str(),
        video_url.c_str(),
        ev.peak_accel_g, ev.min_accel_g,
        ev.peak_gyro_dps, ev.tilt_change_deg,
        local_dir.c_str());
    return buf;
}
