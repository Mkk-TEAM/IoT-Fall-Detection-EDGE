// replay_tool: Feed a saved imu_window.csv back through FallDetector.
//
// Usage:
//   ./replay_tool <imu_window.csv> [fall_detector_config.json]
//
// Output: state transition log + final stats printed to stdout.
// Useful for tuning thresholds without live BLE hardware.

#include "fall/FallDetector.h"
#include "fall/FallConfig.h"
#include "fall/ImuSample.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::vector<ImuSample> load_csv(const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "[replay] Cannot open: %s\n", path.c_str());
        std::exit(1);
    }

    std::vector<ImuSample> samples;
    std::string line;
    std::getline(f, line);  // skip header

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tok;
        std::vector<std::string> cols;
        while (std::getline(ss, tok, ',')) cols.push_back(tok);
        if (cols.size() < 16) continue;

        ImuSample s{};
        s.gw_ms          = std::stoll(cols[0]);
        s.esp_ms         = (uint32_t)std::stoul(cols[1]);
        s.seq            = (uint16_t)std::stoul(cols[2]);
        s.ax             = std::stof(cols[3]);
        s.ay             = std::stof(cols[4]);
        s.az             = std::stof(cols[5]);
        s.gx             = std::stof(cols[6]);
        s.gy             = std::stof(cols[7]);
        s.gz             = std::stof(cols[8]);
        s.roll           = std::stof(cols[9]);
        s.pitch          = std::stof(cols[10]);
        s.yaw            = std::stof(cols[11]);
        s.accel_mag_g    = std::stof(cols[12]);
        s.dynamic_accel_g = std::stof(cols[13]);
        s.gyro_mag_dps   = std::stof(cols[14]);
        s.detector_state = std::stoi(cols[15]);
        samples.push_back(s);
    }
    return samples;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: replay_tool <imu_window.csv> [fall_detector_config.json]\n");
        return 1;
    }

    std::string csv_path    = argv[1];
    std::string config_path = argc >= 3 ? argv[2] : "fall_detector_config.json";

    FallConfig cfg = FallConfig::load(config_path);
    cfg.accel_unit_mps2 = false;  // CSV already stores in g
    cfg.print();

    auto samples = load_csv(csv_path);
    if (samples.empty()) {
        std::fprintf(stderr, "[replay] No samples loaded from: %s\n", csv_path.c_str());
        return 1;
    }
    std::printf("[replay] Loaded %zu samples from %s\n", samples.size(), csv_path.c_str());
    std::printf("[replay] Time span: %lldms → %lldms (%lldms total)\n",
                (long long)samples.front().gw_ms,
                (long long)samples.back().gw_ms,
                (long long)(samples.back().gw_ms - samples.front().gw_ms));

    FallDetector det(cfg);

    int event_count = 0;
    det.set_on_event([&](const FallEvent &ev) {
        ++event_count;
        std::printf("\n[replay] ── EVENT #%d ─────────────────────────────────\n",
                    event_count);
        std::printf("  type       : %s\n", ev.event_type.c_str());
        std::printf("  event_id   : %s\n", ev.event_id.c_str());
        std::printf("  timestamp  : %s\n", ev.timestamp.c_str());
        std::printf("  confidence : %.2f\n", ev.confidence);
        std::printf("  had_freefall: %s\n", ev.had_freefall ? "yes" : "no");
        std::printf("  peak_accel : %.2f g\n", ev.peak_accel_g);
        std::printf("  min_accel  : %.2f g\n", ev.min_accel_g);
        std::printf("  peak_gyro  : %.1f dps\n", ev.peak_gyro_dps);
        std::printf("  tilt_change: %.1f °\n", ev.tilt_change_deg);
        std::printf("  inactivity : %lld ms\n", (long long)ev.inactivity_duration_ms);
        std::printf("  reasons    :");
        for (const auto &r : ev.reasons) std::printf(" %s", r.c_str());
        std::printf("\n  window     : %zu samples\n", ev.window.size());
    });

    for (const auto &s : samples) {
        det.push_sample(s);
        det.tick(s.gw_ms);
    }

    auto stats = det.get_stats();
    std::printf("\n[replay] ── Final stats ──────────────────────────────────\n");
    std::printf("  samples    : %llu\n", (unsigned long long)stats.samples);
    std::printf("  candidates : %llu\n", (unsigned long long)stats.candidates);
    std::printf("  confirmed  : %llu\n", (unsigned long long)stats.confirmed);
    std::printf("  rejected   : %llu\n", (unsigned long long)stats.rejected);
    std::printf("  final_state: %s\n",  det.get_state_str().c_str());
    std::printf("[replay] Done — %d events emitted.\n", event_count);
    return 0;
}
