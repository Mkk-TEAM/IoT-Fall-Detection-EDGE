# IoT-Fall-Detection-EDGE

Raspberry Pi gateway cho hệ thống phát hiện té ngã. Gồm hai process chính:

| Binary / Script | Vai trò | Port |
|---|---|---|
| `edge-streamer` (C++) hoặc `stream_server.py` | MJPEG camera stream | 8081 |
| `edge-imu-ble` (C++) | BLE IMU client → backend | 8082 (health) |
| `record_service.py` | Ghi clip MP4 từ MJPEG | — |
| `imu_simulator.py` | Giả lập IMU (dev/debug only) | — |

---

## Cấu hình

```bash
cp .env.example .env
# Chỉnh BE_BASE_URL, EDGE_SECRET, IMU_DEVICE_ID, GATEWAY_ID, EDGE_PUBLIC_HOST
```

---

## Camera stream

### Pi 5 — dùng Python wrapper (khuyến nghị)

Pi 5 dùng driver `rp1-cfe` — OpenCV V4L2 không đọc trực tiếp. `stream_server.py` wrap `rpicam-vid`.

```bash
sudo usermod -aG video $USER  # một lần
python3 stream_server.py

# Hoặc systemd
sudo cp edge-stream.service /etc/systemd/system/
sudo systemctl enable --now edge-stream.service
```

### Pi 4 — C++ binary

```bash
sudo apt install libopencv-dev libcpp-httplib-dev libssl-dev zlib1g-dev libbrotli-dev pkg-config cmake build-essential
mkdir -p build && cd build
cmake ..
make -j4 edge-streamer
./edge-streamer
```

---

## BLE IMU Client (edge-imu-ble)

Nhận dữ liệu IMU thật từ ESP32 `FallDetect-IMU` qua BLE GATT và POST lên backend.

### Yêu cầu

```bash
# BlueZ + D-Bus (cần cho SimpleBLE trên Linux)
sudo apt install libdbus-1-dev bluetooth bluez cmake build-essential

# Cho phép user dùng BLE không cần sudo
sudo usermod -aG bluetooth $USER
# (logout + login lại để có hiệu lực)
```

### Build

```bash
mkdir -p build && cd build
cmake ..                        # Lần đầu: tải SimpleBLE qua FetchContent (~vài phút)
make -j4 edge-imu-ble
make -j4 imu_packet_parser_test # Parser test, không cần BLE
```

Tắt BLE client nếu không cần (chỉ build camera):
```bash
cmake -DBUILD_BLE_CLIENT=OFF ..
make -j4
```

### Chạy real BLE mode

```bash
# Đảm bảo ESP32 đã power on và đang advertise "FallDetect-IMU"
cd /home/ubuntu/BK/252_DADN/IoT-Fall-Detection-EDGE
./build/edge-imu-ble --imu-source ble
```

Log khi hoạt động bình thường:
```
[BLE] adapter: hci0 [AA:BB:CC:DD:EE:FF]
[BLE] State: SCANNING for "FallDetect-IMU"
[BLE] Found: FallDetect-IMU [11:22:33:44:55:66]
[BLE] State: CONNECTING
[BLE] Connected
[BLE] State: SUBSCRIBING
[BLE] Subscribed to char 12345678-1234-1234-1234-123456789abc on service 12345678-1234-1234-1234-123456789012
[BLE] State: RECEIVING
[BLE] stats: recv=100 valid=100 bad_len=0 bad_magic=0 csum_err=0 reconnects=0 last_seq=99 hz=20.1 age_ms=3
[POST] ...
```

### Chạy simulator mode (dev)

```bash
# Chạy Python simulator riêng
python3 imu_simulator.py
```

Binary `edge-imu-ble` với `--imu-source simulator` chỉ in hướng dẫn và thoát.

### Health endpoint

```bash
curl http://localhost:8082/health
```

```json
{
  "imu_source": "ble",
  "imu_connected": true,
  "state": "RECEIVING",
  "last_packet_age_ms": 47,
  "packets_valid": 1234,
  "checksum_errors": 0,
  "reconnect_count": 0,
  "estimated_hz": 20.1,
  "posts_ok": 246,
  "posts_failed": 0
}
```

### Systemd (autostart)

```bash
sudo cp edge-imu-ble.service /etc/systemd/system/
sudo systemctl enable --now edge-imu-ble.service
sudo journalctl -u edge-imu-ble -f
```

> **Lưu ý:** Chỉ chạy MỘT trong hai: `edge-imu-ble.service` (real) hoặc `edge-imu-sim.service` (giả lập), không chạy cả hai cùng lúc.

---

## Parser unit test

Test parser 61-byte BLE packet không cần phần cứng BLE:

```bash
./build/imu_packet_parser_test
```

Output mong đợi:
```
=== ImuPacket parser tests ===

test_valid_parse
  PASS  returns OK
  PASS  seq
  ...
=== 19 passed, 0 failed ===
```

---

## BLE packet protocol

Packet 61 byte, little-endian:

| Offset | Size | Field | Đơn vị |
|--------|------|-------|--------|
| 0-1 | 2 | magic = `0xAA 0x55` | — |
| 2-3 | 2 | seq (uint16) | — |
| 4-15 | 12 | ax, ay, az (float×3) | m/s² |
| 16-27 | 12 | gx, gy, gz (float×3) | deg/s |
| 28-39 | 12 | roll, pitch, yaw (float×3) | deg |
| 40-55 | 16 | q0, q1, q2, q3 (float×4) | — |
| 56-59 | 4 | esp_ms (uint32) | ms |
| 60 | 1 | checksum = XOR của byte [0..59] | — |

Firmware chưa implement quaternion (q=0 là hợp lệ, parser chấp nhận).

---

## Lỗi thường gặp

### `Bluetooth adapter not available`

```bash
sudo systemctl start bluetooth
hciconfig hci0 up
```

### `Permission denied` khi dùng BLE

```bash
sudo usermod -aG bluetooth $USER
# logout + login lại
# Hoặc chạy với sudo (không khuyến nghị production)
```

### ESP32 không xuất hiện trong scan

- Kiểm tra ESP32 đã power on và LED đang chớp (advertising)
- `bluetoothctl scan on` để xem danh sách device
- Đảm bảo không có device khác đang connect đến ESP32 (BLE GATT 1:1)

### Packet length sai (không phải 61 byte)

- Kiểm tra firmware ESP32 đúng version (struct BlePacket 61 bytes)
- Kiểm tra MTU negotiation: ESP32 set MTU=512, Pi nhận đủ 1 packet

### `SimpleBLE` không tải được (build offline)

```bash
cmake -DBUILD_BLE_CLIENT=OFF ..
make -j4
# Dùng simulator tạm thời
python3 imu_simulator.py
```

---

## Test endpoints

```bash
# Camera stream
curl http://localhost:8081/health
curl -o snapshot.jpg http://localhost:8081/snapshot.jpg

# IMU BLE client
curl http://localhost:8082/health

# Test từ máy khác trong LAN
curl http://<PI_IP>:8081/health
curl http://<PI_IP>:8082/health
```
