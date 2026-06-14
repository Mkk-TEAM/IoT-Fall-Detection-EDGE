#pragma once
#include <string>
#include "../imu/ImuConfig.h"
#include "FallDetector.h"

/// POSTs fall_confirmed events to the backend /internal/events endpoint.
/// Non-confirmed events (fall_rejected) are skipped (returns 0).
class EventPoster {
public:
    explicit EventPoster(const ImuConfig &cfg);

    /// Returns HTTP status code (201 = ok, 0 = skipped, -1 = connection error).
    int post(const FallEvent &ev, const std::string &local_dir);

private:
    std::string build_json(const FallEvent &ev, const std::string &local_dir) const;

    ImuConfig   cfg_;
    std::string be_host_;    // "http://host:port"
    std::string api_prefix_; // "/api/v1"
};
