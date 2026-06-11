#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

std::atomic<bool> g_running{true};
httplib::Server *g_server = nullptr;

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

void load_dotenv(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) return;

  std::string line;
  while (std::getline(file, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;

    const auto separator = line.find('=');
    if (separator == std::string::npos) continue;

    auto key = trim(line.substr(0, separator));
    auto value = trim(line.substr(separator + 1));

    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }

    if (!key.empty() && std::getenv(key.c_str()) == nullptr) {
      setenv(key.c_str(), value.c_str(), 0);
    }
  }
}

int get_env_int(const char *key, int fallback) {
  if (const auto *value = std::getenv(key)) {
    try {
      return std::stoi(value);
    } catch (...) {
      std::cerr << "[WARN] Invalid integer for " << key << ": " << value
                << ". Falling back to " << fallback << '\n';
    }
  }
  return fallback;
}

std::string get_env_string(const char *key, const std::string &fallback) {
  if (const auto *value = std::getenv(key)) {
    return value;
  }
  return fallback;
}

struct AppConfig {
  std::string host = "0.0.0.0";
  int port = 8081;
  std::string camera_device = "/dev/video0";
  int width = 640;
  int height = 480;
  int fps = 15;
  int jpeg_quality = 80;
};

AppConfig load_config() {
  load_dotenv(".env");

  AppConfig config;
  config.host = get_env_string("EDGE_STREAM_HOST", config.host);
  config.port = get_env_int("EDGE_STREAM_PORT", config.port);
  config.camera_device =
      get_env_string("CAMERA_DEVICE", config.camera_device);
  config.width = get_env_int("CAMERA_WIDTH", config.width);
  config.height = get_env_int("CAMERA_HEIGHT", config.height);
  config.fps = get_env_int("CAMERA_FPS", config.fps);
  config.jpeg_quality = get_env_int("JPEG_QUALITY", config.jpeg_quality);
  return config;
}

bool is_numeric_string(const std::string &value) {
  if (value.empty()) return false;
  for (char ch : value) {
    if (ch < '0' || ch > '9') return false;
  }
  return true;
}

bool parse_video_index(const std::string &camera_device, int &index) {
  if (is_numeric_string(camera_device)) {
    index = std::stoi(camera_device);
    return true;
  }

  const std::string prefix = "/dev/video";
  if (camera_device.rfind(prefix, 0) == 0) {
    const auto suffix = camera_device.substr(prefix.size());
    if (is_numeric_string(suffix)) {
      index = std::stoi(suffix);
      return true;
    }
  }

  return false;
}

class CameraService {
public:
  explicit CameraService(AppConfig config) : config_(std::move(config)) {}

  bool open() {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_locked();
  }

  bool camera_opened() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capture_.isOpened();
  }

  bool grab_jpeg(std::vector<unsigned char> &jpeg_buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!capture_.isOpened() && !open_locked()) return false;

    cv::Mat frame;
    capture_ >> frame;
    if (frame.empty()) {
      std::cerr << "[ERROR] Captured empty frame from camera.\n";
      capture_.release();
      return false;
    }

    const std::vector<int> params = {
        cv::IMWRITE_JPEG_QUALITY, config_.jpeg_quality};
    return cv::imencode(".jpg", frame, jpeg_buffer, params);
  }

  std::string health_json() const {
    std::ostringstream output;
    output << "{"
           << "\"status\":\"ok\","
           << "\"cameraOpened\":" << (camera_opened() ? "true" : "false")
           << ",\"width\":" << config_.width << ",\"height\":" << config_.height
           << ",\"fps\":" << config_.fps << "}";
    return output.str();
  }

  const AppConfig &config() const { return config_; }

private:
  bool open_locked() {
    if (capture_.isOpened()) return true;

    int index = 0;
    bool opened = false;

    if (parse_video_index(config_.camera_device, index)) {
      opened = capture_.open(index, cv::CAP_V4L2);
      if (!opened) {
        std::cerr << "[WARN] Failed to open camera by index " << index
                  << " with CAP_V4L2.\n";
      }
    }

    if (!opened) {
      opened = capture_.open(config_.camera_device, cv::CAP_V4L2);
      if (!opened) {
        std::cerr << "[WARN] Failed to open camera path " << config_.camera_device
                  << " with CAP_V4L2.\n";
      }
    }

    if (!opened) {
      opened = capture_.open(config_.camera_device);
      if (!opened) {
        std::cerr << "[ERROR] Unable to open camera device: "
                  << config_.camera_device << '\n';
        return false;
      }
    }

    capture_.set(cv::CAP_PROP_FRAME_WIDTH, config_.width);
    capture_.set(cv::CAP_PROP_FRAME_HEIGHT, config_.height);
    capture_.set(cv::CAP_PROP_FPS, config_.fps);

    std::cerr << "[INFO] Camera opened: " << config_.camera_device << " ("
              << config_.width << "x" << config_.height << " @" << config_.fps
              << "fps)\n";
    return true;
  }

  AppConfig config_;
  mutable std::mutex mutex_;
  cv::VideoCapture capture_;
};

void set_cors_headers(httplib::Response &res) {
  res.set_header("Access-Control-Allow-Origin", "*");
  res.set_header("Access-Control-Allow-Headers", "*");
  res.set_header("Cache-Control", "no-store, no-cache, must-revalidate");
  res.set_header("Pragma", "no-cache");
}

void signal_handler(int) {
  g_running = false;
  if (g_server != nullptr) {
    g_server->stop();
  }
}

} // namespace

int main() {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  auto config = load_config();
  CameraService camera(config);
  camera.open();

  httplib::Server server;
  g_server = &server;
  server.set_keep_alive_max_count(100);
  server.set_keep_alive_timeout(5);
  server.new_task_queue = [] { return new httplib::ThreadPool(8); };

  server.Options(R"(.*)", [](const httplib::Request &, httplib::Response &res) {
    set_cors_headers(res);
    res.status = 204;
  });

  server.Get("/health", [&camera](const httplib::Request &, httplib::Response &res) {
    set_cors_headers(res);
    res.set_content(camera.health_json(), "application/json");
  });

  server.Get("/snapshot.jpg",
             [&camera](const httplib::Request &, httplib::Response &res) {
               set_cors_headers(res);

               std::vector<unsigned char> jpeg;
               if (!camera.grab_jpeg(jpeg)) {
                 res.status = 503;
                 res.set_content(
                     "{\"status\":\"error\",\"message\":\"Camera frame unavailable\"}",
                     "application/json");
                 return;
               }

               res.set_content(reinterpret_cast<const char *>(jpeg.data()),
                               jpeg.size(), "image/jpeg");
             });

  server.Get("/stream.mjpg",
             [&camera](const httplib::Request &, httplib::Response &res) {
               set_cors_headers(res);

               std::vector<unsigned char> first_frame;
               if (!camera.grab_jpeg(first_frame)) {
                 res.status = 503;
                 res.set_content(
                     "{\"status\":\"error\",\"message\":\"Camera stream unavailable\"}",
                     "application/json");
                 return;
               }

               res.set_chunked_content_provider(
                   "multipart/x-mixed-replace; boundary=frame",
                   [&camera, first_frame = std::move(first_frame)](
                       size_t offset, httplib::DataSink &sink) mutable {
                     if (offset == 0) {
                       std::ostringstream header;
                       header << "--frame\r\n"
                              << "Content-Type: image/jpeg\r\n"
                              << "Content-Length: " << first_frame.size()
                              << "\r\n\r\n";

                       if (!sink.write(header.str().c_str(), header.str().size())) {
                         return false;
                       }
                       if (!sink.write(reinterpret_cast<const char *>(first_frame.data()),
                                       first_frame.size())) {
                         return false;
                       }
                       if (!sink.write("\r\n", 2)) {
                         return false;
                       }
                     }

                     while (g_running && sink.is_writable()) {
                       std::vector<unsigned char> jpeg;
                       if (!camera.grab_jpeg(jpeg)) {
                         sink.done();
                         return false;
                       }

                       std::ostringstream header;
                       header << "--frame\r\n"
                              << "Content-Type: image/jpeg\r\n"
                              << "Content-Length: " << jpeg.size() << "\r\n\r\n";

                       if (!sink.write(header.str().c_str(), header.str().size())) {
                         return false;
                       }
                       if (!sink.write(reinterpret_cast<const char *>(jpeg.data()),
                                       jpeg.size())) {
                         return false;
                       }
                       if (!sink.write("\r\n", 2)) {
                         return false;
                       }

                       const auto frame_delay = std::max(1, 1000 / camera.config().fps);
                       std::this_thread::sleep_for(
                           std::chrono::milliseconds(frame_delay));
                     }

                     sink.done();
                     return true;
                   });
             });

  std::cerr << "[INFO] Edge streamer listening on http://" << config.host << ':'
            << config.port << '\n';
  std::cerr << "[INFO] Endpoints: /health, /snapshot.jpg, /stream.mjpg\n";

  if (!server.listen(config.host, config.port)) {
    std::cerr << "[ERROR] Failed to bind edge streamer on " << config.host << ':'
              << config.port << '\n';
    return 1;
  }

  g_server = nullptr;
  return 0;
}
