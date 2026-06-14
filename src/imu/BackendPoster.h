#pragma once
#include "ImuConfig.h"
#include "ImuPacket.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// Posts the latest IMU packet to the backend at a fixed interval.
//
// update() is non-blocking and safe to call from the BLE notification thread.
// A background thread wakes every post_interval_sec and does the actual HTTP POST.
class BackendPoster {
public:
    struct Stats {
        uint64_t posts_attempted = 0;
        uint64_t posts_ok        = 0;
        uint64_t posts_failed    = 0;
    };

    explicit BackendPoster(ImuConfig config);
    ~BackendPoster();

    // Start the background posting thread.
    void start();

    // Stop the background thread. Blocks until it exits.
    void stop();

    // Store the latest packet (non-blocking). Thread-safe.
    void update(const ImuPacket &pkt);

    Stats get_stats() const;

private:
    void thread_func();
    bool do_post(const std::string &path, const std::string &body);

    static std::string build_health_log_json(const ImuPacket &pkt,
                                              const std::string &device_id,
                                              const std::string &gateway_id);
    static std::string utc_now_iso8601();

    ImuConfig    config_;
    std::string  be_host_;    // "http://host:port"
    std::string  api_prefix_; // "/api/v1"

    std::thread          thread_;
    std::atomic<bool>    running_{false};

    mutable std::mutex   pkt_mutex_;
    ImuPacket            latest_pkt_{};
    bool                 has_pkt_ = false;

    mutable std::mutex   stats_mutex_;
    Stats                stats_{};
};
