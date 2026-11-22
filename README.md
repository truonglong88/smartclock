# Smart Clock

Firmware riêng cho màn hình "Smart Weather Clock" mua trên Shopee (bản clone GeekMagic, ESP8266 + ST7789 240x240).

Màn hình hiện: thứ/ngày, giờ chạy theo giây, thời tiết TP.HCM (Open-Meteo), tỷ giá USD/VND.

## Cập nhật hoạt động thế nào

```
sửa firmware/ → push lên main → GitHub Actions build → release/firmware.bin + version.txt
                                                                  ↓
                              màn hình kiểm tra 5 phút/lần (giây ~30) → thấy version mới → tự cài
```

**Phải tăng `FW_VERSION` trong `firmware/smartclock/smartclock.ino`** mỗi khi muốn phát hành, nếu không màn hình sẽ coi là bản cũ.
Không muốn đợi thì vào `http://smartclock.local` → "Kiem tra cap nhat ngay".

## Cấu trúc

| Đường dẫn | Nội dung |
|---|---|
| `firmware/smartclock/` | Code Arduino + `tft_setup.h` (cấu hình chân màn hình) |
| `scripts/build.sh` | Build ra `build/firmware.bin` (chạy được trên Mac/Linux) |
| `release/` | Bản firmware mà màn hình tải về — do GitHub Actions ghi, không sửa tay |
| `tools/smartclock_push.py` | Script đẩy ảnh cho **firmware gốc** (để tham khảo) |

## Phần cứng (đã kiểm chứng)

- ESP8266EX, flash 4MB, USB-serial có sẵn (`/dev/cu.usbserial-*` trên Mac)
- ST7789 240x240: SCK=14, MOSI=13, DC=0, RST=2, không CS, SPI mode 3, đảo màu (INVON)
- Đèn nền GPIO5, **LOW = sáng** (thả nổi → màn hình đen)

## Nạp tay / khôi phục

- Qua web: `http://smartclock.local/update` → chọn `firmware.bin`
- Qua USB: `python -m esptool --port /dev/cu.usbserial-10 --baud 115200 write_flash 0x0 firmware.bin`
- Firmware gốc của hãng: nạp lại file backup `smartclock_backup.bin` (không có trong repo) bằng lệnh USB ở trên
- Cài lại WiFi: `http://smartclock.local/wifi-reset`, rồi kết nối WiFi `SmartClock-Setup` → `192.168.4.1`

## Lưu ý bảo mật

Trang `/update` không có mật khẩu và tải firmware qua HTTPS không kiểm tra chứng chỉ, nên chỉ dùng trong mạng nhà; **không mở port ra internet**.
