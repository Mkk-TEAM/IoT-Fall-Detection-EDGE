#include "fall/FallDetector.h"
#include "fall/FallConfig.h"
#include "fall/ImuSample.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

// ── helpers ────────────────────────────────────────────────────────────────────

static FallConfig make_fast_config() {
    FallConfig c;
    c.freefall_min_ms           = 80;
    c.freefall_impact_window_ms = 500;
    c.post_impact_window_ms     = 600;   // 10s → 600ms for test speed
    c.posture_eval_delay_ms     = 100;   // 500ms → 100ms
    c.cooldown_ms               = 200;
    c.tilt_confirm_deg          = 40.0f;
    c.upright_pitch_deg         = -90.0f;
    c.accel_unit_mps2           = false; // samples already in g
    return c;
}

// pitch=-90 means upright/standing; pitch=0 means lying horizontal (fallen)
static ImuSample make_s(int64_t gw_ms,
                         float accel_g,
                         float gyro_dps = 0.0f,
                         float roll = 0.0f, float pitch = -90.0f) {
    ImuSample s{};
    s.gw_ms           = gw_ms;
    s.accel_mag_g     = accel_g;
    s.dynamic_accel_g = std::fabs(accel_g - 1.0f);
    s.gyro_mag_dps    = gyro_dps;
    s.roll            = roll;
    s.pitch           = pitch;
    return s;
}

static std::vector<FallEvent> run(FallDetector &det,
                                   const std::vector<ImuSample> &samples) {
    std::vector<FallEvent> events;
    det.set_on_event([&](const FallEvent &ev) { events.push_back(ev); });
    for (const auto &s : samples) {
        det.push_sample(s);
        det.tick(s.gw_ms);
    }
    return events;
}

// ── test cases ─────────────────────────────────────────────────────────────────

static void test_still_no_fall() {
    FallDetector det(make_fast_config());
    std::vector<ImuSample> samples;
    // 2s of quiet standing — no impact, pitch=-90 (upright)
    for (int i = 0; i < 100; ++i)
        samples.push_back(make_s(i * 20, 1.0f, 2.0f, 0.0f, -90.0f));

    auto events = run(det, samples);
    assert(events.empty() && "still standing: should emit no events");
    printf("[PASS] test_still_no_fall\n");
}

static void test_stumble_rejected() {
    // Impact, but person stays upright (pitch stays near -90°) → rejected after timeout.
    FallDetector det(make_fast_config());
    std::vector<ImuSample> samples;
    int64_t t = 0;
    // 200ms normal standing
    for (int i = 0; i < 10; ++i) { samples.push_back(make_s(t, 1.0f, 5.0f, 0.0f, -90.0f)); t += 20; }
    // Impact spike
    samples.push_back(make_s(t, 3.5f, 200.0f, 0.0f, -90.0f)); t += 20;
    // Post-impact: pitch stays near upright (-90°±15°) — person caught themselves
    for (int i = 0; i < 40; ++i) { samples.push_back(make_s(t, 1.0f, 10.0f, 2.0f, -85.0f)); t += 20; }

    auto events = run(det, samples);
    assert(!events.empty());
    assert(events.back().event_type == "fall_rejected" && "stumble (stays upright) should be rejected");
    printf("[PASS] test_stumble_rejected\n");
}

static void test_direct_impact_confirmed() {
    // Direct impact + pitch goes to 0° (lying on back) → confirmed immediately after delay.
    FallDetector det(make_fast_config());
    std::vector<ImuSample> samples;
    int64_t t = 0;
    // 300ms normal standing, pitch=-90
    for (int i = 0; i < 15; ++i) { samples.push_back(make_s(t, 1.0f, 5.0f, 0.0f, -90.0f)); t += 20; }
    // Impact
    samples.push_back(make_s(t, 4.2f, 300.0f, 0.0f, -90.0f)); t += 20;
    // Brief bounce with changing pitch
    for (int i = 0; i < 5; ++i) { samples.push_back(make_s(t, 1.5f, 40.0f, 0.0f, -90.0f + 10.0f*i)); t += 20; }
    // Settled on floor: pitch=0° (lying horizontal) — tilt from upright = 90° > 40° → confirm
    for (int i = 0; i < 20; ++i) { samples.push_back(make_s(t, 0.95f, 4.0f, 0.0f, 0.0f)); t += 20; }

    auto events = run(det, samples);
    assert(!events.empty());
    assert(events.back().event_type == "fall_confirmed" && "direct impact + lying → confirm");
    assert(events.back().had_freefall == false);
    assert(events.back().tilt_change_deg > 40.0f && "tilt_from_upright should exceed threshold");
    printf("[PASS] test_direct_impact_confirmed\n");
}

static void test_freefall_impact_confirmed() {
    // Freefall → impact → pitch goes to 10° (nearly lying flat) → confirmed.
    FallDetector det(make_fast_config());
    std::vector<ImuSample> samples;
    int64_t t = 0;
    // Normal standing
    for (int i = 0; i < 10; ++i) { samples.push_back(make_s(t, 1.0f, 5.0f, 0.0f, -90.0f)); t += 20; }
    // Freefall (100ms ≥ freefall_min_ms=80)
    for (int i = 0; i < 5; ++i)  { samples.push_back(make_s(t, 0.2f, 10.0f, 0.0f, -90.0f)); t += 20; }
    // Impact
    samples.push_back(make_s(t, 5.1f, 350.0f, 0.0f, -90.0f)); t += 20;
    // Brief transition
    for (int i = 0; i < 5; ++i)  { samples.push_back(make_s(t, 1.2f, 30.0f, 0.0f, -90.0f + 15.0f*i)); t += 20; }
    // Lying on side: pitch=-10° — tilt from upright = 80° > 40° → confirm
    for (int i = 0; i < 20; ++i) { samples.push_back(make_s(t, 0.9f, 3.0f, 0.0f, -10.0f)); t += 20; }

    auto events = run(det, samples);
    assert(!events.empty());
    assert(events.back().event_type == "fall_confirmed" && "freefall+impact+lying → confirm");
    assert(events.back().had_freefall == true);
    printf("[PASS] test_freefall_impact_confirmed\n");
}

static void test_cooldown_prevents_double_trigger() {
    // Two rapid impacts — second suppressed during cooldown.
    FallDetector det(make_fast_config());
    std::vector<ImuSample> samples;
    int64_t t = 0;

    // First fall
    for (int i = 0; i < 10; ++i) { samples.push_back(make_s(t, 1.0f, 5.0f, 0.0f, -90.0f)); t += 20; }
    samples.push_back(make_s(t, 4.0f, 280.0f, 0.0f, -90.0f)); t += 20;
    for (int i = 0; i < 5; ++i)  { samples.push_back(make_s(t, 1.1f, 20.0f, 0.0f, -90.0f + 18.0f*i)); t += 20; }
    for (int i = 0; i < 20; ++i) { samples.push_back(make_s(t, 0.95f, 3.0f, 0.0f, 0.0f)); t += 20; }

    // Second impact immediately (still in cooldown)
    samples.push_back(make_s(t, 4.5f, 300.0f, 0.0f, 0.0f)); t += 20;
    for (int i = 0; i < 20; ++i) { samples.push_back(make_s(t, 0.95f, 3.0f, 0.0f, 0.0f)); t += 20; }

    auto events = run(det, samples);
    int confirmed = 0;
    for (const auto &ev : events)
        if (ev.event_type == "fall_confirmed") ++confirmed;
    assert(confirmed == 1 && "cooldown should suppress second trigger");
    printf("[PASS] test_cooldown_prevents_double_trigger\n");
}

// ── main ───────────────────────────────────────────────────────────────────────

int main() {
    printf("=== fall_detector_test ===\n");
    test_still_no_fall();
    test_stumble_rejected();
    test_direct_impact_confirmed();
    test_freefall_impact_confirmed();
    test_cooldown_prevents_double_trigger();
    printf("=== All tests passed ===\n");
    return 0;
}
