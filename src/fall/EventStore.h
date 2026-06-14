#pragma once
#include <string>
#include "FallDetector.h"

/// Writes event.json and imu_window.csv to data/events/<event_id>/.
class EventStore {
public:
    explicit EventStore(std::string base_dir);

    /// Saves the event.  Returns the event directory path (empty on error).
    /// be_result is the backend HTTP response string (e.g., "ok", "http_401").
    std::string save(const FallEvent &ev, const std::string &be_result = "");

private:
    void write_csv(const std::string &dir, const FallEvent &ev) const;
    void write_json(const std::string &dir, const FallEvent &ev,
                    const std::string &csv_path,
                    const std::string &be_result) const;

    std::string base_dir_;
};
