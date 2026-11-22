// Smart Clock firmware v1 cho "Smart Weather Clock" (ESP8266 + ST7789 240x240)
// - Đồng hồ chạy từng giây (NTP, giờ Việt Nam)
// - Thời tiết TP.HCM (Open-Meteo, không cần API key)
// - Tỷ giá USD/VND (open.er-api.com)
// - Mắt robot: liếc, chớp, cảm xúc theo thời tiết / giờ
// - Cài WiFi qua điện thoại (WiFiManager), truy cập http://smartclock.local
// - Cập nhật firmware qua web (/update) và TỰ cập nhật từ GitHub (5 phút/lần)

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiClientSecureBearSSL.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <TFT_eSPI.h>
#include <time.h>

#define FW_VERSION "1.2.1"
#define DEFAULT_OTA_URL "https://raw.githubusercontent.com/truonglong88/smartclock/main/release/"
const float LAT = 10.78, LON = 106.70;          // TP.HCM

TFT_eSPI tft;
TFT_eSprite eyeSpr = TFT_eSprite(&tft);
const int SPR_W = 220, SPR_H = 130, SPR_X = 10, SPR_Y = 30;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// ---- trạng thái ----
float tempC = NAN; int humid = -1; int wcode = -1;
float usdVnd = NAN;
unsigned long lastWeather = 0, lastFx = 0, lastOta = 0;
bool otaNow = false;
bool footerDirty = true;
String shownIp;
bool weatherDirty = true, fxDirty = true;
int lastMinute = -1, lastDay = -1;

// ---- cấu hình lưu EEPROM: URL tự cập nhật ----
struct Config { uint32_t magic; char otaUrl[160]; uint8_t mode; } cfg;  // mode: 0 xoay, 1 đồng hồ, 2 mắt
const uint32_t MAGIC = 0x5C10C001;
void loadCfg() {
  EEPROM.begin(sizeof(Config)); EEPROM.get(0, cfg);
  if (cfg.magic != MAGIC) { memset(&cfg, 0, sizeof cfg); cfg.magic = MAGIC; }
  if (!cfg.otaUrl[0]) strncpy(cfg.otaUrl, DEFAULT_OTA_URL, sizeof(cfg.otaUrl) - 1);
  if (cfg.mode > 2) cfg.mode = 0;
}
void saveCfg() { EEPROM.put(0, cfg); EEPROM.commit(); }

// ---- màu ----
#define C_BG     TFT_BLACK
#define C_DATE   0xFEA0
#define C_CLOCK  TFT_WHITE
#define C_BAR    0x2D7F
#define C_WX     0x9FFB
#define C_FX     0x87F0
#define C_DIM    0x7BEF

const char* wxText(int c) {
  if (c == 0) return "Troi quang";
  if (c <= 2) return "It may";
  if (c == 3) return "Nhieu may";
  if (c == 45 || c == 48) return "Suong mu";
  if (c >= 51 && c <= 57) return "Mua phun";
  if (c >= 61 && c <= 67) return "Mua";
  if (c >= 80 && c <= 82) return "Mua rao";
  if (c >= 95) return "Giong bao";
  return "--";
}

void centerText(const String& s, int y, int font, uint16_t fg) {
  tft.setTextDatum(TC_DATUM); tft.setTextColor(fg, C_BG); tft.setTextPadding(240);
  tft.drawString(s, 120, y, font); tft.setTextPadding(0);
}

void statusScreen(const String& l1, const String& l2, const String& l3 = "") {
  tft.fillScreen(C_BG);
  centerText(l1, 70, 4, C_DATE); centerText(l2, 110, 2, C_CLOCK); centerText(l3, 135, 2, C_CLOCK);
}

// ---- lấy dữ liệu ----
void fetchWeather() {
  WiFiClient client; HTTPClient http; http.useHTTP10(true); http.setTimeout(8000);
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + LAT + "&longitude=" + LON +
               "&current=temperature_2m,relative_humidity_2m,weather_code";
  if (http.begin(client, url) && http.GET() == 200) {
    StaticJsonDocument<128> filter; filter["current"] = true;
    DynamicJsonDocument doc(768);
    if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) {
      tempC = doc["current"]["temperature_2m"] | NAN;
      humid = doc["current"]["relative_humidity_2m"] | -1;
      wcode = doc["current"]["weather_code"] | -1;
      weatherDirty = true;
    }
  }
  http.end();
}

void fetchFx() {
  BearSSL::WiFiClientSecure client; client.setInsecure(); client.setBufferSizes(4096, 512);
  HTTPClient http; http.useHTTP10(true); http.setTimeout(10000);
  if (http.begin(client, "https://open.er-api.com/v6/latest/USD") && http.GET() == 200) {
    StaticJsonDocument<64> filter; filter["rates"]["VND"] = true;
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) {
      usdVnd = doc["rates"]["VND"] | NAN; fxDirty = true;
    }
  }
  http.end();
}

// ---- tự cập nhật ----
String otaStatus = "chua kiem tra";
void checkOta() {
  if (!cfg.otaUrl[0]) { otaStatus = "chua dat URL"; return; }
  String base = cfg.otaUrl; if (!base.endsWith("/")) base += "/";
  BearSSL::WiFiClientSecure client; client.setInsecure(); client.setBufferSizes(4096, 512);
  HTTPClient http; http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, base + "version.txt?t=" + String(millis()))) { otaStatus = "loi URL"; return; }
  int code = http.GET();
  String v = code == 200 ? http.getString() : ""; v.trim(); http.end();
  if (code != 200) { otaStatus = "version.txt HTTP " + String(code); return; }
  if (v == FW_VERSION) { otaStatus = "dang moi nhat (" + v + ")"; return; }
  statusScreen("Dang cap nhat", String(FW_VERSION) + " -> " + v, "dung rut dien");
  // Tải firmware cần bộ đệm nhận TLS đủ 16KB: GitHub gửi bản ghi TLS 16KB, bộ đệm nhỏ hơn sẽ đứt giữa chừng
  client.stop();
  eyeSpr.deleteSprite();   // nhả ~3.5KB RAM cho TLS
  BearSSL::WiFiClientSecure dl; dl.setInsecure(); dl.setBufferSizes(16384, 512);
  Serial.printf("[ota] %s -> %s, heap %u\n", FW_VERSION, v.c_str(), ESP.getFreeHeap());
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = ESPhttpUpdate.update(dl, base + "firmware.bin?v=" + v);
  otaStatus = "cap nhat loi: " + ESPhttpUpdate.getLastErrorString();
  Serial.println("[ota] " + otaStatus);
  statusScreen("Cap nhat loi", ESPhttpUpdate.getLastErrorString().substring(0, 30), "giu ban " FW_VERSION);
  delay(5000);
  eyeSpr.createSprite(SPR_W, SPR_H);
  lastMinute = lastDay = -1; weatherDirty = fxDirty = true; tft.fillScreen(C_BG);
  (void)r;
}

// ---- web ----
void handleRoot() {
  time_t now = time(nullptr);
  String h = F("<!doctype html><meta name=viewport content='width=device-width'><style>body{font-family:sans-serif;max-width:520px;margin:20px auto;padding:0 12px}input{width:100%;padding:8px;box-sizing:border-box}button{padding:8px 14px;margin-top:8px}</style><h2>Smart Clock</h2>");
  h += "<p>Firmware <b>" FW_VERSION "</b> &middot; IP " + WiFi.localIP().toString() + " &middot; RSSI " + WiFi.RSSI() + " dBm &middot; RAM trong " + ESP.getFreeHeap() + " B</p>";
  h += "<p>Nhiet do: " + String(tempC, 1) + " C, do am " + humid + "% &middot; USD/VND: " + String(usdVnd, 0) + "</p>";
  const char* modes[] = {"Xoay vong (dong ho 40s / mat 20s)", "Chi dong ho", "Chi mat robot"};
  h += "<h3>Che do hien thi</h3><form method=post action=/mode>";
  for (int i = 0; i < 3; i++) h += "<label><input type=radio name=m value=" + String(i) + (cfg.mode == i ? " checked" : "") + " style='width:auto'> " + modes[i] + "</label><br>";
  h += "<button>Luu</button></form>";
  h += "<h3>Tu cap nhat</h3><p>Trang thai: " + otaStatus + "</p><form method=post action=/save><label>URL thu muc chua <code>version.txt</code> va <code>firmware.bin</code></label><input name=ota value='" + String(cfg.otaUrl) + "' placeholder='https://raw.githubusercontent.com/ban/smartclock/main/release/'><button>Luu</button></form>";
  h += "<form method=post action=/check><button>Kiem tra cap nhat ngay</button></form>";
  h += "<h3>Nap firmware bang tay</h3><p><a href=/update>/update</a> &middot; <a href=/wifi-reset onclick=\"return confirm('Xoa WiFi?')\">Xoa WiFi</a></p>";
  (void)now;
  server.send(200, "text/html; charset=utf-8", h);
}

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, []() {
    String u = server.arg("ota"); u.trim();
    strncpy(cfg.otaUrl, u.c_str(), sizeof(cfg.otaUrl) - 1); cfg.otaUrl[sizeof(cfg.otaUrl) - 1] = 0; saveCfg();
    server.sendHeader("Location", "/"); server.send(303);
  });
  server.on("/mode", HTTP_POST, []() {
    int m = server.arg("m").toInt(); if (m >= 0 && m <= 2) { cfg.mode = m; saveCfg(); }
    server.sendHeader("Location", "/"); server.send(303);
  });
  server.on("/check", HTTP_POST, []() { server.sendHeader("Location", "/"); server.send(303); otaNow = true; });
  server.on("/wifi-reset", []() { server.send(200, "text/plain", "Da xoa WiFi, khoi dong lai..."); delay(500); WiFiManager wm; wm.resetSettings(); ESP.restart(); });
  server.on("/api", []() {
    String j = String("{\"fw\":\"" FW_VERSION "\",\"temp\":") + (isnan(tempC) ? String("null") : String(tempC, 1)) +
               ",\"humid\":" + humid + ",\"usd_vnd\":" + (isnan(usdVnd) ? String("null") : String(usdVnd, 0)) + "}";
    server.send(200, "application/json", j);
  });
  httpUpdater.setup(&server, "/update");
  server.begin();
}

// ---- vẽ ----
const char* DOW[] = {"CN", "T2", "T3", "T4", "T5", "T6", "T7"};

void drawDegree(int x, int y, uint16_t col) { tft.drawCircle(x, y, 4, col); tft.drawCircle(x, y, 3, col); }

void drawScreen() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;  // chưa có giờ NTP
  struct tm t; localtime_r(&now, &t);

  if (t.tm_mday != lastDay) {
    char d[24]; snprintf(d, sizeof d, "%s  %02d/%02d/%04d", DOW[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    centerText(d, 6, 4, C_DATE); lastDay = t.tm_mday;
  }
  if (t.tm_min != lastMinute) {
    char c[6]; snprintf(c, sizeof c, "%02d:%02d", t.tm_hour, t.tm_min);
    tft.setTextDatum(TC_DATUM); tft.setTextColor(C_CLOCK, C_BG);
    tft.setTextSize(1); tft.drawString(c, 120, 44, 7);
    tft.fillRect(20, 104, 200, 4, 0x2104);
    lastMinute = t.tm_min;
  }
  // thanh giây
  int w = (t.tm_sec * 200) / 59;
  tft.fillRect(20, 104, w, 4, C_BAR);
  if (w < 200) tft.fillRect(20 + w, 104, 200 - w, 4, 0x2104);

  if (weatherDirty) {
    tft.fillRect(0, 120, 240, 56, C_BG);
    if (!isnan(tempC)) {
      String s = String((int)round(tempC));
      tft.setTextDatum(TL_DATUM); tft.setTextColor(C_WX, C_BG);
      int tw = tft.drawString(s, 18, 124, 6);
      drawDegree(18 + tw + 6, 130, C_WX);
      tft.drawString("C", 18 + tw + 13, 124, 4);
      tft.drawString(wxText(wcode), 120, 126, 4);
      tft.setTextColor(C_DIM, C_BG);
      tft.drawString("Do am " + String(humid) + "%", 120, 154, 2);
    } else {
      centerText("Dang lay thoi tiet...", 140, 2, C_DIM);
    }
    weatherDirty = false;
  }
  if (fxDirty) {
    String s = isnan(usdVnd) ? String("USD  ...") : "USD  " + String((long)round(usdVnd));
    if (!isnan(usdVnd)) {  // chèn dấu phẩy hàng nghìn
      String n = String((long)round(usdVnd)), o; int k = 0;
      for (int i = n.length() - 1; i >= 0; i--) { o = n[i] + o; if (++k % 3 == 0 && i > 0) o = "," + o; }
      s = "USD  " + o;
    }
    centerText(s, 186, 4, C_FX); fxDirty = false;
    footerDirty = true;
  }
}

void drawFooter() {
  String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("mat WiFi...");
  if (!footerDirty && ip == shownIp) return;
  centerText(ip + "   v" FW_VERSION, 222, 2, C_DIM);
  shownIp = ip; footerDirty = false;
}

// ================= MẮT ROBOT =================
struct Eye { float x, y, h, w, tilt, happy; };
Eye cur = {0, 0, 90, 70, 0, 0}, tgt = cur;
unsigned long nextLook = 0, nextBlink = 0, blinkStart = 0, surpriseUntil = 0;
int eyeMinuteShown = -1;
enum Page { PAGE_CLOCK, PAGE_EYES } page = PAGE_CLOCK;
unsigned long pageSince = 0;
int lastHourSeen = -1;

bool isNight(const struct tm& t) { return t.tm_hour >= 21 || t.tm_hour < 6; }

void eyesPickTarget(const struct tm& t) {
  unsigned long m = millis();
  bool night = isNight(t), rain = wcode >= 51, surprised = m < surpriseUntil;
  tgt.x = random(-28, 29); tgt.y = random(-14, 15);
  tgt.w = 70; tgt.h = 90; tgt.tilt = 0; tgt.happy = 0;
  if (surprised)      { tgt.w = 84; tgt.h = 110; tgt.x = 0; tgt.y = -6; }
  else if (night)     { tgt.h = 34; tgt.y = 14; tgt.x = random(-10, 11); }
  else if (rain)      { tgt.tilt = 1; tgt.y = random(0, 15); }
  else if (random(100) < 18) { tgt.happy = 1; tgt.x = random(-12, 13); }
  nextLook = m + (night ? random(3000, 7000) : random(1200, 4000));
}

void eyesFrame(const struct tm& t) {
  unsigned long m = millis();
  if (m >= nextLook) eyesPickTarget(t);
  float k = (m < surpriseUntil) ? 0.45f : 0.22f;
  cur.x += (tgt.x - cur.x) * k; cur.y += (tgt.y - cur.y) * k;
  cur.w += (tgt.w - cur.w) * k; cur.h += (tgt.h - cur.h) * k;
  cur.tilt += (tgt.tilt - cur.tilt) * k; cur.happy += (tgt.happy - cur.happy) * k;

  // chớp mắt
  float hDraw = cur.h;
  if (!blinkStart && m >= nextBlink) blinkStart = m;
  if (blinkStart) {
    unsigned long d = m - blinkStart;
    if (d < 90) hDraw = cur.h * (1 - d / 90.0f) + 4;
    else if (d < 180) hDraw = 4 + (cur.h - 4) * ((d - 90) / 90.0f);
    else { blinkStart = 0; nextBlink = m + (random(100) < 20 ? 250 : random(2000, 6000)); }  // thỉnh thoảng chớp đôi
  }

  eyeSpr.fillSprite(0);
  for (int side = 0; side < 2; side++) {
    int cx = (side == 0 ? 58 : 162) + (int)cur.x, cy = SPR_H / 2 + (int)cur.y;
    int w = (int)cur.w, h = max(4, (int)hDraw), r = min(20, h / 2);
    int x0 = cx - w / 2, y0 = cy - h / 2;
    eyeSpr.fillRoundRect(x0, y0, w, h, r, 1);
    if (cur.tilt > 0.05f) {  // mí buồn: xệ phía ngoài
      int drop = (int)(cur.tilt * h * 0.5f);
      if (side == 0) eyeSpr.fillTriangle(x0 - 2, y0 - 2, x0 + w + 2, y0 - 2, x0 - 2, y0 + drop, 0);
      else           eyeSpr.fillTriangle(x0 - 2, y0 - 2, x0 + w + 2, y0 - 2, x0 + w + 2, y0 + drop, 0);
    }
    if (cur.happy > 0.05f && h > 20) {  // mắt cười ^ ^
      int eh = (int)(h * 0.55f * cur.happy);
      eyeSpr.fillEllipse(cx, y0 + h + eh / 3, w * 0.75f, eh, 0);
    }
  }
  bool night = isNight(t);
  eyeSpr.setBitmapColor(night ? 0xFB20 : 0x07FF, TFT_BLACK);  // cam ban đêm, xanh ngọc ban ngày
  eyeSpr.pushSprite(SPR_X, SPR_Y);

  if (t.tm_min != eyeMinuteShown) {
    char c[6]; snprintf(c, sizeof c, "%02d:%02d", t.tm_hour, t.tm_min);
    centerText(c, 180, 4, night ? 0x7A00 : C_DIM);
    eyeMinuteShown = t.tm_min;
  }
}

void enterPage(Page p) {
  page = p; pageSince = millis();
  tft.fillScreen(C_BG);
  lastMinute = lastDay = -1; eyeMinuteShown = -1;
  weatherDirty = fxDirty = footerDirty = true;
}

void updatePage(const struct tm& t) {
  unsigned long m = millis();
  // đầu mỗi giờ: giật mình 6 giây (trừ ban đêm)
  if (t.tm_min == 0 && t.tm_hour != lastHourSeen && !isNight(t) && cfg.mode != 1) {
    lastHourSeen = t.tm_hour; surpriseUntil = m + 6000; nextLook = 0;
    if (page != PAGE_EYES) enterPage(PAGE_EYES);
    return;
  }
  if (m < surpriseUntil) return;
  Page want = page;
  if (cfg.mode == 1) want = PAGE_CLOCK;
  else if (cfg.mode == 2) want = PAGE_EYES;
  else want = (page == PAGE_CLOCK && m - pageSince > 40000) ? PAGE_EYES
            : (page == PAGE_EYES && m - pageSince > 20000) ? PAGE_CLOCK : page;
  if (want != page) enterPage(want);
}

void setup() {
  Serial.begin(115200); delay(200);
  Serial.println("\n[boot] Smart Clock " FW_VERSION);
  loadCfg();
  pinMode(5, OUTPUT); digitalWrite(5, LOW);   // đèn nền (pha A của pintest)
  Serial.println("[boot] tft.init");
  tft.init(); tft.setRotation(0);
  // chớp màu để xác nhận màn hình chạy
  tft.fillScreen(TFT_RED); delay(400); tft.fillScreen(TFT_GREEN); delay(400); tft.fillScreen(TFT_BLUE); delay(400);
  tft.fillScreen(C_BG);
  eyeSpr.setColorDepth(1);
  if (!eyeSpr.createSprite(SPR_W, SPR_H)) Serial.println("[eyes] sprite FAIL");
  randomSeed(ESP.getChipId() ^ micros());
  Serial.println("[boot] tft ok");
  statusScreen("Smart Clock", "Dang ket noi WiFi...", "v" FW_VERSION);

  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);   // không ngủ modem: web/OTA luôn trả lời được
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  wm.setAPCallback([](WiFiManager*) {
    statusScreen("Cai WiFi", "Dien thoai ket noi WiFi:", "SmartClock-Setup");
    centerText("roi mo 192.168.4.1", 160, 2, C_CLOCK);
  });
  Serial.println("[wifi] autoConnect");
  if (!wm.autoConnect("SmartClock-Setup")) ESP.restart();
  Serial.print("[wifi] IP "); Serial.println(WiFi.localIP());

  configTime("<+07>-7", "pool.ntp.org", "time.google.com");
  MDNS.begin("smartclock");
  setupWeb();
  MDNS.addService("http", "tcp", 80);
  statusScreen("Da ket noi", WiFi.localIP().toString(), "http://smartclock.local");
  delay(2500);
  tft.fillScreen(C_BG);
  otaNow = true;  // kiểm tra cập nhật ngay khi vừa khởi động
}

void loop() {
  server.handleClient();
  MDNS.update();
  unsigned long m = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (!lastWeather || m - lastWeather > 15UL * 60 * 1000) { lastWeather = m; fetchWeather(); }
    if (!lastFx || m - lastFx > 60UL * 60 * 1000) { lastFx = m; fetchFx(); }
    // kiểm tra cập nhật 5 phút/lần, vào giây ~30 để không trùng lúc đổi phút
    time_t now = time(nullptr); struct tm t; localtime_r(&now, &t);
    bool slot = now > 1700000000 && t.tm_min % 5 == 2 && t.tm_sec >= 30 && m - lastOta > 120000UL;
    if (otaNow || slot) { otaNow = false; lastOta = m; checkOta(); }
  }
  static unsigned long lastDraw = 0;
  time_t nowT = time(nullptr); struct tm tt; localtime_r(&nowT, &tt);
  bool timeOk = nowT > 1700000000;
  if (timeOk) updatePage(tt);
  if (page == PAGE_EYES && timeOk) {
    if (m - lastDraw >= 33) { lastDraw = m; eyesFrame(tt); drawFooter(); }   // ~30 fps
  } else if (m - lastDraw >= 250) { lastDraw = m; drawScreen(); drawFooter(); }
  // tự kết nối lại nếu rớt WiFi quá 30s
  static unsigned long lostSince = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (!lostSince) lostSince = m;
    if (m - lostSince > 30000) { Serial.println("[wifi] reconnect"); WiFi.reconnect(); lostSince = m; }
  } else lostSince = 0;
}
