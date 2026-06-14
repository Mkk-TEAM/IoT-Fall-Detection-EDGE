#include "FallDetector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <random>

// ── helpers ───────────────────────────────────────────────────────────────────

const char *fall_state_str(FallState s) {
    switch (s) {
    case FallState::NORMAL:          return "NORMAL";
    case FallState::FREEFALL:        return "FREEFALL";
    case FallState::IMPACT_DETECTED: return "IMPACT_DETECTED";
    case FallState::POST_IMPACT:     return "POST_IMPACT";
    case FallState::FALL_CONFIRMED:  return "FALL_CONFIRMED";
    case FallState::FALL_REJECTED:   return "FALL_REJECTED";
    case FallState::COOLDOWN:        return "COOLDOWN";
    }
    return "UNKNOWN";
}

std::string FallDetector::make_event_id() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<unsigned> d(0, 0xFFFFu);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "evt_%lld_%04x", (long long)ms, d(rng));
    return buf;
}

std::string FallDetector::utc_now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count() % 1000;
    struct tm t{};
    gmtime_r(&tt, &t);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        t.tm_year+1900, t.tm_mon+1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec, (long long)ms);
    return buf;
}

// ── construction ──────────────────────────────────────────────────────────────

FallDetector::FallDetector(FallConfig cfg)
    : cfg_(std::move(cfg))
    , ring_(size_t(cfg_.sample_rate_hz_expected) * size_t(cfg_.ring_buffer_seconds))
{}

void FallDetector::set_on_event(EventCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    on_event_ = std::move(cb);
}

FallState   FallDetector::get_state() const {
    std::lock_guard<std::mutex> lk(mu_); return state_;
}
std::string FallDetector::get_state_str() const {
    std::lock_guard<std::mutex> lk(mu_); return fall_state_str(state_);
}
FallDetector::Stats FallDetector::get_stats() const {
    std::lock_guard<std::mutex> lk(mu_); return stats_;
}

// ── push_sample ───────────────────────────────────────────────────────────────

void FallDetector::push_sample(const ImuSample &input) {
    FallEvent pending;
    bool has_event = false;

    {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.samples;

        ImuSample s = input;
        s.detector_state = static_cast<int>(state_);
        ring_.push(s);

        switch (state_) {
        case FallState::NORMAL:      on_normal(s);          break;
        case FallState::FREEFALL:    on_freefall(s);        break;
        case FallState::POST_IMPACT: on_post_impact(s);     break;
        case FallState::COOLDOWN:    on_cooldown(s.gw_ms);  break;
        default: break;
        }

        if (state_ == FallState::POST_IMPACT) {
            if (try_finish_evaluation(s.gw_ms, pending)) has_event = true;
        }
    }

    if (has_event && on_event_) on_event_(pending);
}

// ── tick (timer thread) ───────────────────────────────────────────────────────

void FallDetector::tick(int64_t now_ms) {
    FallEvent pending;
    bool has_event = false;

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == FallState::POST_IMPACT) {
            if (try_finish_evaluation(now_ms, pending)) has_event = true;
        } else if (state_ == FallState::COOLDOWN) {
            on_cooldown(now_ms);
        }
    }

    if (has_event && on_event_) on_event_(pending);
}

// ── state handlers ────────────────────────────────────────────────────────────

void FallDetector::on_normal(const ImuSample &s) {
    if (s.accel_mag_g >= cfg_.impact_g) {
        in_ff_ = false; ff_wait_impact_ = false;
        ++stats_.candidates;
        start_candidate(s, false);
        return;
    }
    if (s.accel_mag_g <= cfg_.freefall_g) {
        if (!in_ff_) { in_ff_ = true; ff_start_ = s.gw_ms; }
        if ((s.gw_ms - ff_start_) >= cfg_.freefall_min_ms) {
            ++stats_.candidates;
            transition(FallState::FREEFALL, "freefall sustained");
        }
    } else {
        in_ff_ = false;
    }
}

void FallDetector::on_freefall(const ImuSample &s) {
    if (s.accel_mag_g <= cfg_.freefall_g) return;

    if (!ff_wait_impact_) {
        ff_wait_impact_ = true;
        ff_exit_ = s.gw_ms;
        std::printf("[FallDetector] Freefall exit seq=%u accel=%.2fg — waiting impact\n",
                    s.seq, s.accel_mag_g);
    }
    if (s.accel_mag_g >= cfg_.impact_g) {
        ff_wait_impact_ = false; in_ff_ = false;
        start_candidate(s, true);
        return;
    }
    if ((s.gw_ms - ff_exit_) > cfg_.freefall_impact_window_ms) {
        std::printf("[FallDetector] Freefall impact window expired → NORMAL\n");
        ff_wait_impact_ = false; in_ff_ = false;
        transition(FallState::NORMAL, "freefall impact window expired");
    }
}

void FallDetector::on_post_impact(const ImuSample &s) {
    post_buf_.push_back(s);
    if (s.accel_mag_g  > peak_acc_)  peak_acc_  = s.accel_mag_g;
    if (s.accel_mag_g  < min_acc_)   min_acc_   = s.accel_mag_g;
    if (s.gyro_mag_dps > peak_gyro_) peak_gyro_ = s.gyro_mag_dps;
}

void FallDetector::on_cooldown(int64_t now_ms) {
    if (now_ms >= cooldown_end_) {
        in_ff_ = false; ff_wait_impact_ = false;
        transition(FallState::NORMAL, "cooldown expired");
    }
}

// ── start_candidate ───────────────────────────────────────────────────────────

void FallDetector::start_candidate(const ImuSample &s, bool from_ff) {
    had_ff_    = from_ff;
    impact_ms_ = s.gw_ms;
    peak_acc_  = s.accel_mag_g;
    min_acc_   = s.accel_mag_g;
    peak_gyro_ = s.gyro_mag_dps;
    post_start_ = s.gw_ms;
    post_buf_.clear();

    ImuSample tagged = s;
    tagged.detector_state = static_cast<int>(FallState::IMPACT_DETECTED);
    post_buf_.push_back(tagged);

    std::printf("[FallDetector] Candidate: seq=%u accel=%.2fg freefall=%s pitch=%.1f\n",
                s.seq, s.accel_mag_g, from_ff ? "yes" : "no", s.pitch);

    transition(FallState::IMPACT_DETECTED, from_ff ? "freefall+impact" : "direct_impact");
    transition(FallState::POST_IMPACT,     "monitoring post-impact");
}

// ── try_finish_evaluation ──────────────────────────────────────────────────────
//
// New posture-based logic:
//   1. During posture_eval_delay_ms: ignore (bounce settling).
//   2. Each sample after delay: compute tilt from upright (-90°).
//      - If tilt > tilt_confirm_deg → FALL_CONFIRMED immediately.
//   3. If post_impact_window_ms expires without confirmation
//      → person stayed upright → FALL_REJECTED.

bool FallDetector::try_finish_evaluation(int64_t now_ms, FallEvent &out) {
    int64_t elapsed = now_ms - post_start_;

    // Settling period: bounce from impact, don't evaluate yet.
    if (elapsed < cfg_.posture_eval_delay_ms) return false;

    // Compute tilt from upright for the most recent sample.
    float tilt = 0.0f;
    if (!post_buf_.empty()) {
        tilt = tilt_from_upright(post_buf_.back().pitch);
        if (tilt > cfg_.tilt_confirm_deg) {
            // Person is no longer upright → confirmed fall.
            return _do_finish(now_ms, tilt, true, out);
        }
    }

    // Still upright: wait for the full monitoring window before rejecting.
    if (elapsed < cfg_.post_impact_window_ms) return false;

    // Timeout: person remained upright → reject (stumble / false positive).
    return _do_finish(now_ms, tilt, false, out);
}

bool FallDetector::_do_finish(int64_t now_ms, float tilt, bool confirmed, FallEvent &out) {
    std::printf("[FallDetector] Eval: tilt_from_upright=%.1f° (thr=%.1f°) peak=%.2fg → %s\n",
                tilt, cfg_.tilt_confirm_deg, peak_acc_,
                confirmed ? "CONFIRMED" : "REJECTED");

    out.event_id   = make_event_id();
    out.event_type = confirmed ? "fall_confirmed" : "fall_rejected";
    out.timestamp  = utc_now_iso8601();
    out.impact_gw_ms       = impact_ms_;
    out.peak_accel_g       = peak_acc_;
    out.min_accel_g        = min_acc_;
    out.peak_gyro_dps      = peak_gyro_;
    out.tilt_change_deg    = tilt;
    out.inactivity_duration_ms = 0;
    out.had_freefall       = had_ff_;
    out.confidence         = compute_confidence(tilt);

    if (had_ff_)   out.reasons.push_back("freefall");
    out.reasons.push_back("impact");
    if (confirmed) out.reasons.push_back("posture_not_upright");
    else           out.reasons.push_back("stayed_upright_10s");

    int64_t pre_from   = impact_ms_ - (int64_t)cfg_.save_pre_event_seconds  * 1000;
    int64_t post_until = impact_ms_ + (int64_t)cfg_.save_post_event_seconds * 1000;
    out.window = ring_.slice_time(pre_from, impact_ms_ - 1);
    for (const auto &ps : post_buf_)
        if (ps.gw_ms <= post_until) out.window.push_back(ps);

    if (confirmed) { ++stats_.confirmed; transition(FallState::FALL_CONFIRMED, "posture not upright"); }
    else           { ++stats_.rejected;  transition(FallState::FALL_REJECTED,  "stayed upright"); }

    cooldown_end_ = now_ms + cfg_.cooldown_ms;
    transition(FallState::COOLDOWN, "post-event cooldown");
    return true;
}

// ── internal helpers ──────────────────────────────────────────────────────────

void FallDetector::transition(FallState next, const char *reason) {
    std::printf("[FallDetector] %s → %s (%s)\n",
                fall_state_str(state_), fall_state_str(next), reason);
    state_ = next;
}

float FallDetector::tilt_from_upright(float pitch) const {
    float d = std::fabs(pitch - cfg_.upright_pitch_deg);
    if (d > 180.0f) d = 360.0f - d;
    return d;
}

float FallDetector::compute_confidence(float tilt) const {
    float s = 0.30f;                   // impact is always present
    if (had_ff_) s += 0.20f;          // freefall detected before impact
    // tilt contribution: 0..0.50 scaled against 2× threshold
    s += 0.50f * std::min(1.0f, tilt / (cfg_.tilt_confirm_deg * 2.0f));
    return std::min(1.0f, s);
}
