#pragma once
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

/// All tunable thresholds for fall detection.
/// Loaded from a JSON file at runtime — no rebuild needed to tune thresholds.
struct FallConfig {
    // ── Input units ───────────────────────────────────────────────────────────
    bool accel_unit_mps2 = true;

    // ── Ring buffer ───────────────────────────────────────────────────────────
    int sample_rate_hz_expected = 50;
    int ring_buffer_seconds     = 20;

    // ── Impact / freefall ─────────────────────────────────────────────────────
    float impact_g                  = 2.5f;
    float freefall_g                = 0.5f;
    int   freefall_min_ms           = 120;
    int   freefall_impact_window_ms = 1000;

    // ── Posture analysis ──────────────────────────────────────────────────────
    int   pre_impact_baseline_ms = 800;  // kept for CSV window building only
    int   posture_eval_delay_ms  = 500;  // skip first N ms after impact (bounce settling)
    float upright_pitch_deg      = -90.0f;  // sensor pitch when person is standing
    float tilt_confirm_deg       = 40.0f;   // tilt from upright → confirm immediately

    // ── Post-impact monitoring window ─────────────────────────────────────────
    // If person stays upright for this long → reject (stumble/false positive).
    int   post_impact_window_ms = 10000;

    // ── Cooldown ──────────────────────────────────────────────────────────────
    int   cooldown_ms = 10000;

    // ── Local data storage ────────────────────────────────────────────────────
    int   save_pre_event_seconds  = 5;
    int   save_post_event_seconds = 5;
    std::string event_data_dir    = "data/events";

    // ── Factory ──────────────────────────────────────────────────────────────

    static FallConfig defaults() { return {}; }

    /// Load from a flat JSON file.  Missing keys use defaults.
    static FallConfig load(const std::string &path) {
        FallConfig cfg;
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << "[FallConfig] " << path << " not found — using defaults\n";
            return cfg;
        }
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        auto extract = [&](const std::string &key) -> std::string {
            std::string needle = "\"" + key + "\"";
            auto pos = content.find(needle);
            if (pos == std::string::npos) return {};
            pos = content.find(':', pos + needle.size());
            if (pos == std::string::npos) return {};
            ++pos;
            while (pos < content.size() &&
                   (content[pos] == ' ' || content[pos] == '\t')) ++pos;
            if (pos >= content.size()) return {};
            if (content[pos] == '"') {
                auto s = pos + 1;
                auto e = content.find('"', s);
                return e == std::string::npos ? "" : content.substr(s, e - s);
            }
            auto e = pos;
            while (e < content.size() && content[e] != ',' && content[e] != '}'
                   && content[e] != '\n' && content[e] != '\r') ++e;
            std::string v = content.substr(pos, e - pos);
            v.erase(std::remove_if(v.begin(), v.end(), ::isspace), v.end());
            return v;
        };

        auto gf = [&](const char *k, float &v) {
            auto s = extract(k); if (!s.empty()) try { v = std::stof(s); } catch (...) {}
        };
        auto gi = [&](const char *k, int &v) {
            auto s = extract(k); if (!s.empty()) try { v = std::stoi(s); } catch (...) {}
        };
        auto gb = [&](const char *k, bool &v) {
            auto s = extract(k); if (s == "true") v = true; else if (s == "false") v = false;
        };
        auto gs = [&](const char *k, std::string &v) {
            auto s = extract(k); if (!s.empty()) v = s;
        };

        gb("accel_unit_mps2",           cfg.accel_unit_mps2);
        gi("sample_rate_hz_expected",   cfg.sample_rate_hz_expected);
        gi("ring_buffer_seconds",       cfg.ring_buffer_seconds);
        gf("impact_g",                  cfg.impact_g);
        gf("freefall_g",                cfg.freefall_g);
        gi("freefall_min_ms",           cfg.freefall_min_ms);
        gi("freefall_impact_window_ms", cfg.freefall_impact_window_ms);
        gi("pre_impact_baseline_ms",    cfg.pre_impact_baseline_ms);
        gi("posture_eval_delay_ms",     cfg.posture_eval_delay_ms);
        gf("upright_pitch_deg",         cfg.upright_pitch_deg);
        gf("tilt_confirm_deg",          cfg.tilt_confirm_deg);
        gi("post_impact_window_ms",     cfg.post_impact_window_ms);
        gi("cooldown_ms",               cfg.cooldown_ms);
        gi("save_pre_event_seconds",    cfg.save_pre_event_seconds);
        gi("save_post_event_seconds",   cfg.save_post_event_seconds);
        gs("event_data_dir",            cfg.event_data_dir);

        return cfg;
    }

    void print() const {
        std::printf(
            "[FallConfig] accel_unit_mps2=%s  ring=%ds @ %dHz\n"
            "[FallConfig] impact_g=%.2f  freefall_g=%.2f  freefall_min=%dms  ff_window=%dms\n"
            "[FallConfig] upright_pitch=%.1fdeg  tilt_confirm=%.1fdeg  posture_delay=%dms\n"
            "[FallConfig] post_window=%dms  cooldown=%dms  pre=%ds post=%ds  data_dir=%s\n",
            accel_unit_mps2 ? "true" : "false", ring_buffer_seconds, sample_rate_hz_expected,
            impact_g, freefall_g, freefall_min_ms, freefall_impact_window_ms,
            upright_pitch_deg, tilt_confirm_deg, posture_eval_delay_ms,
            post_impact_window_ms, cooldown_ms,
            save_pre_event_seconds, save_post_event_seconds, event_data_dir.c_str());
    }
};
