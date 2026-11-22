#!/usr/bin/env python3
"""
Đẩy ảnh tự vẽ lên màn hình "Smart Weather Clock" (ESP8266, firmware gốc).

API tìm được từ web UI của máy:
  POST /upload                      multipart, field "imageFile", tên file file1.jpg..file5.jpg (JPEG 240x240)
  GET  /fileN-switch?getstate=1|0   bật/tắt ảnh N trong vòng xoay Photo
  GET  /themeselect?getstate=N      0=Classic 1=Simple 2=Photo 3=Dial
  GET  /delaytime?...               (tốc độ xoay ảnh 3–60s, chỉnh trong trang /photo)

Cài:  pip install pillow requests
Chạy: python smartclock_push.py            # đẩy 1 lần
      python smartclock_push.py --loop 300 # cập nhật mỗi 5 phút
"""
import argparse, io, time, datetime, warnings
warnings.filterwarnings("ignore", message=".*OpenSSL.*")  # cảnh báo LibreSSL vô hại trên macOS
import requests
from PIL import Image, ImageDraw, ImageFont

HOST = "http://192.168.0.105"   # đổi nếu IP thay đổi (nên đặt IP tĩnh trong router)
SLOT = 5                        # dùng ô ảnh số 5

FONTS_BOLD = ["/System/Library/Fonts/Supplemental/Arial Bold.ttf", "DejaVuSans-Bold.ttf", "arialbd.ttf"]
FONTS_REG = ["/System/Library/Fonts/Supplemental/Arial.ttf", "DejaVuSans.ttf", "arial.ttf"]

def font(size, bold=False):
    for name in (FONTS_BOLD if bold else FONTS_REG):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default(size)

def center(d, y, text, f, fill):
    w = d.textlength(text, font=f)
    d.text(((240 - w) / 2, y), text, font=f, fill=fill)

def get_extra_lines():
    """Chỗ để gắn dữ liệu thật: tỷ giá, AQI, số user KinSuite, lịch họp..."""
    try:
        r = requests.get("https://open.er-api.com/v6/latest/USD", timeout=5).json()
        return [f"USD/VND {r['rates']['VND']:,.0f}"]
    except Exception:
        return ["(chưa lấy được dữ liệu)"]

def render():
    img = Image.new("RGB", (240, 240))
    d = ImageDraw.Draw(img)
    for y in range(240):  # nền gradient
        t = y / 239
        d.line([(0, y), (240, y)], fill=(int(15 + 29 * t), int(32 + 51 * t), int(39 + 61 * t)))
    now = datetime.datetime.now()
    thu = ["Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy", "Chủ Nhật"][now.weekday()]
    center(d, 22, "Xin chào Long!", font(22, True), (255, 209, 102))
    center(d, 62, now.strftime("%H:%M"), font(56, True), (255, 255, 255))
    center(d, 132, f"{thu}, {now:%d/%m}", font(18), (200, 220, 235))
    for i, line in enumerate(get_extra_lines()):
        center(d, 168 + i * 20, line, font(17), (140, 255, 220))  # chừa ~30px dưới cùng: firmware tự in IP đè lên
    return img

def call(method, path, timeout=5, **kw):
    """Firmware hay trả response hỏng (khai Content-Length dài hơn thực tế rồi đóng kết nối).
    Lệnh vẫn được thực hiện, nên chỉ kiểm tra status code và bỏ qua lỗi đọc body."""
    try:
        r = requests.request(method, HOST + path, timeout=timeout, stream=True, **kw)
        r.close()
        if r.status_code != 200:
            raise RuntimeError(f"{path} HTTP {r.status_code}")
    except (requests.exceptions.ChunkedEncodingError, requests.exceptions.ConnectionError) as e:
        if "IncompleteRead" not in str(e):
            raise

def push(img):
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=85)
    name = f"file{SLOT}.jpg"
    call("POST", "/upload", files={"imageFile": (name, buf.getvalue(), "image/jpeg")}, timeout=15)
    time.sleep(1)  # cho ESP8266 ghi flash xong
    for n in range(1, 6):  # chỉ để ô của mình hiện, tắt các ảnh mẫu khác
        call("GET", f"/file{n}-switch", params={"getstate": 1 if n == SLOT else 0})
    call("GET", "/themeselect", params={"getstate": 2})  # chuyển sang theme Photo
    print(time.strftime("%H:%M:%S"), "đã đẩy", name)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--loop", type=int, default=0, help="số giây giữa các lần cập nhật (0 = chạy 1 lần)")
    ap.add_argument("--save", help="chỉ lưu ảnh ra file để xem trước, không đẩy")
    a = ap.parse_args()
    while True:
        img = render()
        if a.save:
            img.save(a.save); print("đã lưu", a.save); break
        try:
            push(img)
        except Exception as e:  # máy mất mạng/khởi động lại: báo lỗi rồi thử lại vòng sau
            print(time.strftime("%H:%M:%S"), "lỗi:", e)
            if not a.loop:
                raise
        if not a.loop:
            break
        time.sleep(a.loop)
