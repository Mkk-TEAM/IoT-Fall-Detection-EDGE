# IoT-Fall-Detection-EDGE

MVP edge streamer cho Raspberry Pi / gateway. Service này mở camera local và
publish MJPEG để frontend trong LAN xem trực tiếp.

## Endpoints

- `GET /health`
- `GET /snapshot.jpg`
- `GET /stream.mjpg`

## Cấu hình

Copy `.env.example` thành `.env` rồi chỉnh:

```env
EDGE_STREAM_HOST=0.0.0.0
EDGE_STREAM_PORT=8081
CAMERA_DEVICE=/dev/video0
CAMERA_WIDTH=640
CAMERA_HEIGHT=480
CAMERA_FPS=15
JPEG_QUALITY=80
```

## Chạy trên Pi 5 (khuyến nghị) — `stream_server.py`

Pi 5 dùng driver `rp1-cfe` — OpenCV V4L2 không đọc được trực tiếp.
Dùng `stream_server.py` thay thế; nó wrap `rpicam-vid` qua subprocess.

```bash
# Thêm user vào group video (một lần)
sudo usermod -aG video $USER

# Chạy thủ công
python3 stream_server.py

# Hoặc dùng systemd service (autostart)
sudo cp edge-stream.service /etc/systemd/system/
sudo systemctl enable --now edge-stream.service
```

## Build C++ (Pi 4 / V4L2 standard)

```bash
mkdir -p build
cd build
cmake ..
make -j4
```

## Run C++ binary

```bash
cd build
./edge-streamer
```

## Test

Kiểm tra camera:

```bash
ls /dev/video*
v4l2-ctl --list-devices
libcamera-hello --list-cameras
```

Test local:

```bash
curl http://localhost:8081/health
curl -o snapshot.jpg http://localhost:8081/snapshot.jpg
```

Test từ máy khác trong LAN:

```text
http://<PI_IP>:8081/health
http://<PI_IP>:8081/stream.mjpg
```

`/health` luôn trả JSON, và `cameraOpened=false` nếu camera chưa mở được.
