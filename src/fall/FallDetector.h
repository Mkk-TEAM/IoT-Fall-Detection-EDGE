#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "FallConfig.h"
#include "ImuSample.h"
#include "RingBuffer.h"

// ── State machine states ──────────────────────────────────────────────────────

enum class FallState : int {
    NORMAL           = 0,
    FREEFALL         = 1,
    IMPACT_DETECTED  = 2,   // transient: one transition step before POST_IMPACT
    POST_IMPACT      = 3,
    FALL_CONFIRMED   = 4,
    FALL_REJECTED    = 5,
    COOLDOWN         = 6,
};

const char *fall_state_str(FallState s);

// ── Fall event emitted by detector ───────────────────────────────────────────

struct FallEvent {
    std::string event_id;
    std::string event_type;        // "fall_confirmed" | "fall_rejected" | "fall_candidate"
    int64_t     impact_gw_ms = 0;
    std::string timestamp;         // ISO-8601 UTC

    // Metrics
    float   peak_accel_g           = 0;
    float   min_accel_g            = 0;
    float   peak_gyro_dps          = 0;
    float   tilt_change_deg        = 0;
    int64_t inactivity_duration_ms = 0;
    float   confidence             = 0;
    bool    had_freefall           = false;

    std::vector<std::string> reasons;
    std::vector<ImuSample>   window;   // pre + post samples for CSV
};

// ── Detector ─────────────────────────────────────────────────────────────────

class FallDetector {
public:
    using EventCallback = std::function<void(const FallEvent &)>;

    explicit FallDetector(FallConfig cfg);

    void set_on_event(EventCallback cb);

    /// Feed one IMU sample.  Thread-safe; may be called from BLE notification thread.
    void push_sample(const ImuSample &s);

    /// Call from a timer thread (~100 ms interval) to handle post-impact timeout
    /// when BLE samples stop arriving (e.g., disconnect during monitoring).
    void tick(int64_t now_ms);

    FallState   get_state()     const;
    std::string get_state_str() const;

    struct Stats {
        uint64_t samples    = 0;
        uint64_t candidates = 0;
        uint64_t confirmed  = 0;
        uint64_t rejected   = 0;
    };
    Stats get_stats() const;

private:
    // All private methods are called while mu_ is held.
    void on_normal(const ImuSample &s);
    void on_freefall(const ImuSample &s);
    void on_post_impact(const ImuSample &s);
    void on_cooldown(int64_t now_ms);

    void start_candidate(const ImuSample &s, bool from_freefall);
    bool try_finish_evaluation(int64_t now_ms, FallEvent &out);
    bool _do_finish(int64_t now_ms, float tilt, bool confirmed, FallEvent &out);

    void  transition(FallState next, const char *reason);
    float compute_confidence(float tilt_from_upright) const;
    float tilt_from_upright(float pitch) const;

    static std::string make_event_id();
    static std::string utc_now_iso8601();

    // ── data ──────────────────────────────────────────────────────────────────
    FallConfig             cfg_;
    EventCallback          on_event_;
    RingBuffer<ImuSample>  ring_;

    mutable std::mutex mu_;

    FallState state_  = FallState::NORMAL;

    // Freefall tracking
    bool    in_ff_    = false;
    int64_t ff_start_ = 0;
    int64_t ff_exit_  = 0;
    bool    ff_wait_impact_ = false;

    // Impact / candidate tracking
    bool    had_ff_    = false;
    int64_t impact_ms_ = 0;
    float   peak_acc_  = 0;
    float   min_acc_   = 0;
    float   peak_gyro_ = 0;
    float   pre_roll_  = 0;
    float   pre_pitch_ = 0;

    // Post-impact accumulation
    int64_t              post_start_    = 0;
    std::vector<ImuSample> post_buf_;

    // Cooldown
    int64_t cooldown_end_ = 0;

    Stats stats_;
};
