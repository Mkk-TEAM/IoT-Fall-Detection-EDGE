#include "EventStore.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

EventStore::EventStore(std::string base_dir) : base_dir_(std::move(base_dir)) {
    fs::create_directories(base_dir_);
}

std::string EventStore::save(const FallEvent &ev, const std::string &be_result) {
    std::string dir = base_dir_ + "/" + ev.event_id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "[EventStore] mkdir failed: " << ec.message() << '\n';
        return {};
    }
    std::string csv_path = dir + "/imu_window.csv";
    write_csv(dir, ev);
    write_json(dir, ev, csv_path, be_result);
    std::printf("[EventStore] Saved %s → %s  (window=%zu samples)\n",
                ev.event_type.c_str(), dir.c_str(), ev.window.size());
    return dir;
}

void EventStore::write_csv(const std::string &dir, const FallEvent &ev) const {
    std::ofstream f(dir + "/imu_window.csv");
    f << "gateway_time_ms,esp_ms,seq,"
         "ax,ay,az,gx,gy,gz,roll,pitch,yaw,"
         "accel_mag_g,dynamic_accel_g,gyro_mag_dps,detector_state\n";
    for (const auto &s : ev.window) {
        char row[320];
        std::snprintf(row, sizeof(row),
            "%lld,%u,%u,"
            "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,"
            "%.5f,%.5f,%.5f,%d\n",
            (long long)s.gw_ms, s.esp_ms, (unsigned)s.seq,
            s.ax, s.ay, s.az, s.gx, s.gy, s.gz,
            s.roll, s.pitch, s.yaw,
            s.accel_mag_g, s.dynamic_accel_g, s.gyro_mag_dps,
            s.detector_state);
        f << row;
    }
}

void EventStore::write_json(const std::string &dir, const FallEvent &ev,
                             const std::string &csv_path,
                             const std::string &be_result) const {
    // Build reasons JSON array manually (no external dep).
    std::string reasons_json = "[";
    for (size_t i = 0; i < ev.reasons.size(); ++i) {
        if (i) reasons_json += ',';
        reasons_json += '"'; reasons_json += ev.reasons[i]; reasons_json += '"';
    }
    reasons_json += ']';

    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "{\n"
        "  \"event_id\": \"%s\",\n"
        "  \"event_type\": \"%s\",\n"
        "  \"timestamp\": \"%s\",\n"
        "  \"impact_gw_ms\": %lld,\n"
        "  \"had_freefall\": %s,\n"
        "  \"metrics\": {\n"
        "    \"peak_accel_g\": %.3f,\n"
        "    \"min_accel_g\": %.3f,\n"
        "    \"peak_gyro_dps\": %.1f,\n"
        "    \"tilt_change_deg\": %.1f,\n"
        "    \"inactivity_duration_ms\": %lld,\n"
        "    \"confidence\": %.3f\n"
        "  },\n"
        "  \"reasons\": %s,\n"
        "  \"samples_in_window\": %zu,\n"
        "  \"imu_window_file\": \"%s\",\n"
        "  \"backend_post_result\": \"%s\"\n"
        "}\n",
        ev.event_id.c_str(),
        ev.event_type.c_str(),
        ev.timestamp.c_str(),
        (long long)ev.impact_gw_ms,
        ev.had_freefall ? "true" : "false",
        ev.peak_accel_g, ev.min_accel_g, ev.peak_gyro_dps,
        ev.tilt_change_deg, (long long)ev.inactivity_duration_ms,
        ev.confidence,
        reasons_json.c_str(),
        ev.window.size(),
        csv_path.c_str(),
        be_result.c_str());

    std::ofstream f(dir + "/event.json");
    f << buf;
}
