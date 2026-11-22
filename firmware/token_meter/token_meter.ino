/*
 * 額度顯示器 — LILYGO T-QT Pro（ESP32-S3FH4R2，0.85 吋 GC9107 128×128）
 * 2026-11-21（T-Display S3 版見 git 標籤 tdisplay-s3-v1）
 *
 * 從 Mac 的資料服務讀 http://<Mac>.local:8817/meter.json（server/meter_server.py），
 * 每 60 秒更新一次，四頁輪播（每頁 8 秒）：
 *   1. Gemini 組：週額度、5 小時額度＋重置倒數
 *   2. Claude／GPT 組：同上（週額度用完時 5 小時那條變灰）
 *   3. 其他：Antigravity 生圖剩餘、Colab 運算單元、Claude Code 今日用量
 *   4. 連線狀態（2026-11-22 起共六頁：Gemini、Claude／GPT、其他、Antigravity 模型、其他模型、連線）
 * 板子只連 Mac，不碰 Google、不放 API 金鑰；Wi-Fi 等個人設定在 secrets.h（見 secrets.example.h）。
 *
 * 按鍵：左鍵（IO0）＝下一頁；右鍵（IO47）＝暫停／恢復輪播。
 * 驗收：GET http://token-meter.local/screen.bmp?page=0~3 取回畫面。
 * OTA（都要 OTA_PASS）：espota.py -a 密碼；或瀏覽器開 http://token-meter.local/update 上傳 .bin（帳號 admin）。
 * 螢幕驅動：GC9107 初始化序列、偏移 (2,1)、背光低電位亮，沿用已實機驗證的寫法；畫面先畫在 LGFX_Sprite 再整張送出。
 * 中文字：俐方體11號 Cubic 11（OFL），tools/make_u8g2_font.py 產生 cubic11_meter.h。
 */
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <SPI.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#define U8G2_FONT_SECTION(name)
#include "cubic11_meter.h"
#include "meter_types.h"
#include "secrets.h"     // WIFI_SSID、WIFI_PASS、OTA_PASS、MBP_HOST、MBP_FALLBACK_IP

// ───── 設定 ─────
static IPAddress MBP_FALLBACK(MBP_FALLBACK_IP);
static const uint16_t MBP_PORT = 8817;
static const uint32_t FETCH_MS = 60000;
static const uint32_t PAGE_MS = 8000;
static const int W = 128, H = 128, PAGES = 6;

// ───── 螢幕（T-QT Pro）─────
#define PIN_MOSI 2
#define PIN_SCLK 3
#define PIN_CS 5
#define PIN_DC 6
#define PIN_RST 1
#define PIN_BL 10        // 低電位亮
#define PIN_KEY1 0
#define PIN_KEY2 47
#define COL0 2
#define ROW0 1
static SPIClass lcdSpi(FSPI);
LGFX_Sprite fb;
lgfx::U8g2font zh(u8g2_font_cubic11_meter);
WebServer web(80);

static void lcdCmd(uint8_t c, const uint8_t* d = nullptr, size_t n = 0) {
  digitalWrite(PIN_DC, 0); digitalWrite(PIN_CS, 0); lcdSpi.transfer(c); digitalWrite(PIN_CS, 1);
  if (n) { digitalWrite(PIN_DC, 1); digitalWrite(PIN_CS, 0); lcdSpi.writeBytes(d, n); digitalWrite(PIN_CS, 1); }
}
static void lcdCmd1(uint8_t c, uint8_t v) { lcdCmd(c, &v, 1); }

static void lcdInit() {
  pinMode(PIN_CS, OUTPUT); pinMode(PIN_DC, OUTPUT); pinMode(PIN_RST, OUTPUT); pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_CS, 1); digitalWrite(PIN_BL, 0);
  lcdSpi.begin(PIN_SCLK, -1, PIN_MOSI, -1);
  lcdSpi.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_RST, 1); delay(20); digitalWrite(PIN_RST, 0); delay(50); digitalWrite(PIN_RST, 1); delay(120);
  lcdCmd(0xFE); lcdCmd(0xEF);
  const uint8_t regs[][2] = {{0xB0, 0xC0}, {0xB1, 0x80}, {0xB2, 0x27}, {0xB3, 0x13}, {0xB6, 0x19}, {0xB7, 0x05}, {0xAC, 0xC8},
                             {0xAB, 0x0F}, {0x3A, 0x05}, {0xB4, 0x04}, {0xA8, 0x08}, {0xB8, 0x08}, {0xEA, 0x02}, {0xE8, 0x2A},
                             {0xE9, 0x47}, {0xE7, 0x5F}, {0xC6, 0x21}, {0xC7, 0x15}};
  for (auto& r : regs) lcdCmd1(r[0], r[1]);
  const uint8_t g1[] = {0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C, 0x04, 0x12, 0x14, 0x1F};
  const uint8_t g2[] = {0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D, 0x0C, 0x1A, 0x14, 0x1E};
  const uint8_t f4[] = {0x00, 0x00, 0xFF}, ba[] = {0xFF, 0xFF};
  lcdCmd(0xF0, g1, sizeof(g1)); lcdCmd(0xF1, g2, sizeof(g2)); lcdCmd(0xF4, f4, 3); lcdCmd(0xBA, ba, 2);
  lcdCmd(0x20); lcdCmd1(0x36, 0x08);
  lcdCmd(0x11); delay(120); lcdCmd(0x29); delay(20);
}

// 整張送出：Sprite 內部是位元組交換過的 RGB565，正好是 GC9107 要的順序
void flush() {
  const uint8_t ca[] = {0, COL0, 0, COL0 + 127}, ra[] = {0, ROW0, 0, ROW0 + 127};
  lcdCmd(0x2A, ca, 4); lcdCmd(0x2B, ra, 4);
  digitalWrite(PIN_DC, 0); digitalWrite(PIN_CS, 0); lcdSpi.transfer(0x2C);
  digitalWrite(PIN_DC, 1); lcdSpi.writeBytes((uint8_t*)fb.getBuffer(), W * H * 2); digitalWrite(PIN_CS, 1);
}

// 顏色
const uint16_t C_BG = 0x0000, C_FG = 0xFFFF, C_DIM = 0x8410, C_LINE = 0x2945,
               C_GREEN = 0x2FE6, C_YEL = 0xFE60, C_RED = 0xF9A6, C_CYAN = 0x5E7F, C_TRACK = 0x18E3;

// ───── 資料 ─────
struct Meter {
  bool ok = false;
  bool zoneBlocked = false; String zoneWhy;
  bool agyOk = false; uint32_t agyFetched = 0;
  Bucket gWk, g5h, tWk, t5h;
  bool imgOk = false; int imgLeft = -1; bool imgEx = false;
  bool claudeOk = false; long claudeCalls = 0; double claudeTok = 0;
  bool colabOk = false; double colabBal = 0; int colabActive = 0;
  String mAgy[12], mClaude[8], mLocal[6]; int nAgy = 0, nClaude = 0, nLocal = 0;
} M;
int32_t epochOffset = 0;        // 伺服器時間 − millis()/1000
uint32_t lastOkMs = 0, lastTryMs = 0;
String lastErr = "尚未連線";
IPAddress mbpIP;
int page = 0; bool autoPlay = true; uint32_t pageAt = 0;

uint32_t nowEpoch() { return epochOffset ? (uint32_t)(millis() / 1000 + epochOffset) : 0; }

// ───── 繪圖輔助 ─────
void zhText(const String& s, int x, int y, uint16_t col) {
  fb.setFont(&zh); fb.setTextSize(1); fb.setTextColor(col);
  fb.setTextDatum(lgfx::top_left); fb.drawString(s, x, y);
}
int zhWidth(const String& s) { fb.setFont(&zh); fb.setTextSize(1); return fb.textWidth(s); }
void zhRight(const String& s, int xr, int y, uint16_t col) { zhText(s, xr - zhWidth(s), y, col); }
void numRight(const String& s, int xr, int y, uint16_t col) {   // 大數字（只有 ASCII）
  fb.setFont(&fonts::FreeSansBold9pt7b); fb.setTextSize(1); fb.setTextColor(col);
  fb.setTextDatum(lgfx::top_right); fb.drawString(s, xr, y);
}
uint16_t pctColor(float p) { return p >= 50 ? C_GREEN : (p >= 20 ? C_YEL : C_RED); }

String countdown(uint32_t reset) {
  uint32_t n = nowEpoch();
  if (!reset || !n) return "";
  if (reset <= n) return "已重置";
  uint32_t s = reset - n, d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  if (d) return String(d) + "天" + h + "時";
  if (h) return String(h) + "時" + m + "分";
  return String(m) + "分";
}

String ago(uint32_t t) {
  uint32_t n = nowEpoch();
  if (!t || !n || t > n) return "—";
  uint32_t s = n - t;
  if (s < 90) return "剛剛";
  if (s < 3600) return String(s / 60) + "分前";
  return String(s / 3600) + "時前";
}

void header(const String& title, uint16_t col) {
  fb.fillRect(0, 0, W, 15, C_LINE);
  zhText(title, 3, 2, col);
  String r = !M.ok ? "離線" : (M.zoneBlocked ? "暫停" : ago(M.agyFetched));
  zhRight(r, W - 3, 2, (!M.ok || M.zoneBlocked) ? C_YEL : C_DIM);
  for (int i = 0; i < PAGES; i++) fb.fillCircle(W / 2 - (PAGES - 1) * 3 + i * 6, H - 2, 1, i == page ? C_FG : C_DIM);
}

// 一段：標籤＋百分比、橫條、重置倒數
void meterBlock(const char* label, const Bucket& b, int y, bool capped) {
  zhText(label, 3, y + 3, C_DIM);
  if (!b.has) { zhRight("無資料", W - 3, y + 3, C_DIM); return; }
  if (capped) zhRight("週已用完", W - 3, y + 3, C_RED);
  else numRight(String((int)round(b.pct)) + "%", W - 3, y, C_FG);
  fb.fillRoundRect(3, y + 19, W - 6, 10, 3, C_TRACK);
  int w = (int)((W - 6) * constrain(b.pct, 0.0f, 100.0f) / 100.0f);
  if (w > 0) fb.fillRoundRect(3, y + 19, max(w, 6), 10, 3, capped ? C_DIM : pctColor(b.pct));
  String cd = capped ? String("") : (b.pct >= 99.95f ? String("滿") : ("重置 " + countdown(b.reset)));
  zhText(cd, 3, y + 32, C_DIM);
}

// ───── 各頁 ─────
void pageGroup(const char* name, const Bucket& wk, const Bucket& h5) {
  header(name, C_CYAN);
  meterBlock("週額度", wk, 17, false);
  meterBlock("5 小時", h5, 66, wk.has && wk.pct < 1);
}

String fmtTokNum(double t) {
  if (t >= 1e8) return String(t / 1e8, 2);
  if (t >= 1e4) return String((long)round(t / 1e4));
  return String((long)t);
}
const char* fmtTokUnit(double t) { return t >= 1e8 ? "億" : (t >= 1e4 ? "萬" : ""); }

void pageOther() {
  header("其他", C_CYAN);
  zhText("生圖剩", 3, 21, C_DIM);
  if (M.imgOk) { zhRight("張", W - 3, 21, C_DIM); numRight(String(M.imgLeft), W - 17, 18, M.imgLeft > 3 ? C_GREEN : C_YEL); }
  else zhRight("—", W - 3, 21, C_DIM);
  zhText("Colab", 3, 43, C_DIM);
  numRight(M.colabOk ? String(M.colabBal, 0) : String("-"), W - 3, 40, M.colabOk ? C_GREEN : C_DIM);
  fb.drawFastHLine(3, 62, W - 6, C_LINE);
  zhText("Claude 今日用量", 3, 66, C_DIM);
  if (M.claudeOk) {
    String u = fmtTokUnit(M.claudeTok);
    int uw = u.length() ? zhWidth(u) + 2 : 0;
    if (uw) zhRight(u, W - 3, 85, C_DIM);
    numRight(fmtTokNum(M.claudeTok), W - 3 - uw, 82, C_FG);
    zhText("token", 3, 85, C_DIM);
    numRight(String(M.claudeCalls), W - 17, 102, C_FG);
    zhRight("次", W - 3, 105, C_DIM);
    zhText("呼叫", 3, 105, C_DIM);
  } else zhText("無資料", 3, 85, C_DIM);
}

void listPage(const char* title, const char* sub1, String* a, int na, const char* sub2, String* b, int nb) {
  header(title, C_CYAN);
  int y = 17;
  if (sub1) { zhText(sub1, 3, y, C_YEL); y += 12; }
  for (int i = 0; i < na && y < H - 14; i++, y += 12) zhText(a[i], 6, y, C_FG);
  if (sub2 && nb) {
    y += 2; zhText(sub2, 3, y, C_YEL); y += 12;
    for (int i = 0; i < nb && y < H - 14; i++, y += 12) zhText(b[i], 6, y, C_FG);
  }
  if (!na && !nb) zhText("無資料", 3, 30, C_DIM);
}

void pageStatus() {
  header("連線", C_CYAN);
  int y = 18;
  auto row = [&](const String& s, uint16_t c) { zhText(s, 3, y, c); y += 17; };
  bool wf = WiFi.status() == WL_CONNECTED;
  row(wf ? ("Wi-Fi " + String(WiFi.RSSI()) + " dBm") : "Wi-Fi 未連線", wf ? C_FG : C_RED);
  row(WiFi.localIP().toString(), C_FG);
  row("Mac " + (mbpIP ? mbpIP.toString() : String("—")), M.ok ? C_FG : C_YEL);
  row(lastOkMs ? ("成功 " + String((millis() - lastOkMs) / 1000) + " 秒前") : "尚未成功", C_FG);
  row(M.ok ? (M.zoneBlocked ? M.zoneWhy : "正常") : lastErr, M.ok && !M.zoneBlocked ? C_GREEN : C_YEL);
  row(autoPlay ? "輪播中" : "輪播暫停", C_DIM);
}

void render() {
  fb.fillScreen(C_BG);
  if (page == 0) pageGroup("Gemini", M.gWk, M.g5h);
  else if (page == 1) pageGroup("Claude／GPT", M.tWk, M.t5h);
  else if (page == 2) pageOther();
  else if (page == 3) listPage("Antigravity", nullptr, M.mAgy, M.nAgy, nullptr, nullptr, 0);
  else if (page == 4) listPage("其他模型", "Claude Code", M.mClaude, M.nClaude, "本機 Ollama", M.mLocal, M.nLocal);
  else pageStatus();
  flush();
}

void bootScreen(const String& msg) {
  fb.fillScreen(C_BG);
  zhText("額度顯示器", 3, 40, C_CYAN);
  zhText(msg, 3, 62, C_DIM);
  flush();
}

// ───── 取資料 ─────
void readBucket(JsonVariant v, Bucket& b) {
  b.has = !v.isNull();
  if (b.has) { b.pct = v["pct"] | 0.0f; b.reset = v["reset"] | 0u; }
}

void resolveMbp() {
  IPAddress ip = MDNS.queryHost(MBP_HOST, 1500);
  if (ip != IPAddress(0, 0, 0, 0)) mbpIP = ip;
  else if (!mbpIP) mbpIP = MBP_FALLBACK;
}

void fetch() {
  lastTryMs = millis();
  if (WiFi.status() != WL_CONNECTED) { M.ok = false; lastErr = "Wi-Fi 未連線"; return; }
  if (!mbpIP || !M.ok) resolveMbp();
  HTTPClient http;
  http.setTimeout(6000);
  String url = "http://" + mbpIP.toString() + ":" + MBP_PORT + "/meter.json";
  if (!http.begin(url)) { M.ok = false; lastErr = "無法連線"; return; }
  int code = http.GET();
  if (code != 200) {
    M.ok = false; lastErr = code < 0 ? "Mac 沒回應" : ("HTTP " + String(code));
    http.end(); mbpIP = IPAddress(); return;
  }
  JsonDocument d;
  DeserializationError e = deserializeJson(d, http.getString());
  http.end();
  if (e) { M.ok = false; lastErr = "資料格式錯"; return; }
  uint32_t now = d["now"] | 0u;
  if (now) epochOffset = (int32_t)(now - millis() / 1000);
  M.zoneBlocked = d["zone"]["blocked"] | false;
  M.zoneWhy = (const char*)(d["zone"]["why"] | "");
  JsonVariant a = d["agy"];
  M.agyOk = a["ok"] | false; M.agyFetched = a["fetched"] | 0u;
  readBucket(a["gemini_wk"], M.gWk); readBucket(a["gemini_5h"], M.g5h);
  readBucket(a["third_wk"], M.tWk);  readBucket(a["third_5h"], M.t5h);
  JsonVariant im = d["img"];
  M.imgOk = (im["ok"] | false) && !im["left"].isNull();
  M.imgLeft = im["left"] | -1; M.imgEx = im["exhausted"] | false;
  JsonVariant c = d["claude"];
  M.claudeOk = c["ok"] | false; M.claudeCalls = c["calls"] | 0L; M.claudeTok = c["billable"] | 0.0;
  JsonVariant co = d["colab"];
  M.colabOk = co["ok"] | false; M.colabBal = co["balance"] | 0.0; M.colabActive = co["active"] | 0;
  JsonVariant mo = d["models"];
  auto fill = [](JsonVariant arr, String* dst, int cap, int& n) {
    n = 0; for (JsonVariant v : arr.as<JsonArray>()) { if (n >= cap) break; dst[n++] = (const char*)(v | ""); }
  };
  fill(mo["agy"], M.mAgy, 12, M.nAgy); fill(mo["claude"], M.mClaude, 8, M.nClaude); fill(mo["local"], M.mLocal, 6, M.nLocal);
  M.ok = true; lastOkMs = millis(); lastErr = "";
}

// ───── 截圖（驗收用）─────
void handleBmp() {
  render();
  const uint32_t rowBytes = W * 3, img = rowBytes * H, size = 54 + img;   // 128×3 已是 4 的倍數
  uint8_t hdr[54] = {'B', 'M'};
  auto put32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) hdr[o + i] = v >> (8 * i); };
  put32(2, size); put32(10, 54); put32(14, 40); put32(18, W); put32(22, H);
  hdr[26] = 1; hdr[28] = 24; put32(34, img);
  WiFiClient cl = web.client();
  cl.print("HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: " + String(size) + "\r\nConnection: close\r\n\r\n");
  cl.write(hdr, 54);
  uint8_t row[W * 3];
  for (int y = H - 1; y >= 0; y--) {
    for (int x = 0; x < W; x++) {
      uint16_t p = fb.readPixel(x, y);
      row[x * 3 + 0] = (p & 0x1F) << 3; row[x * 3 + 1] = ((p >> 5) & 0x3F) << 2; row[x * 3 + 2] = (p >> 11) << 3;
    }
    cl.write(row, rowBytes);
  }
}

// ───── OTA ─────
void otaProgress(int pct) {
  fb.fillRect(3, 90, W - 6, 8, C_TRACK);
  fb.fillRect(3, 90, (W - 6) * pct / 100, 8, C_CYAN);
  flush();
}

const char OTA_PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>額度顯示器 更新</title><body style="font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em">
<h2>額度顯示器　韌體更新</h2><p>選擇 Arduino 編譯出的 <code>token_meter.ino.bin</code>（不是 merged.bin）。</p>
<form method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="fw" accept=".bin">
<p><button>上傳並更新</button></p></form><p>更新完成會自動重新開機，約 30 秒。</p></body>)HTML";

void setupOta() {
  ArduinoOTA.setHostname("token-meter"); ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([] { bootScreen("OTA 更新中"); });
  ArduinoOTA.onProgress([](unsigned int d, unsigned int t) { otaProgress(d * 100 / t); });
  ArduinoOTA.begin();
  web.on("/update", HTTP_GET, [] {
    if (!web.authenticate("admin", OTA_PASS)) return web.requestAuthentication();
    web.send_P(200, "text/html; charset=utf-8", OTA_PAGE);
  });
  web.on("/update", HTTP_POST, [] {
    if (!web.authenticate("admin", OTA_PASS)) return web.requestAuthentication();
    bool ok = !Update.hasError();
    web.send(200, "text/plain; charset=utf-8", ok ? "更新完成，重新開機中" : "更新失敗，請重試");
    delay(800); if (ok) ESP.restart();
  }, [] {
    if (!web.authenticate("admin", OTA_PASS)) return;
    HTTPUpload& u = web.upload();
    if (u.status == UPLOAD_FILE_START) { bootScreen("網頁更新中"); Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (u.status == UPLOAD_FILE_WRITE) { Update.write(u.buf, u.currentSize); }
    else if (u.status == UPLOAD_FILE_END) { Update.end(true); }
  });
}

// ───── 主程式 ─────
void setup() {
  Serial.begin(115200);
  pinMode(PIN_KEY1, INPUT_PULLUP); pinMode(PIN_KEY2, INPUT_PULLUP);
  lcdInit();
  fb.setColorDepth(16);
  if (!fb.createSprite(W, H)) Serial.println("畫面緩衝建立失敗");
  bootScreen("連線 Wi-Fi");
  WiFi.mode(WIFI_STA); WiFi.setHostname("token-meter");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  MDNS.begin("token-meter");
  setupOta();
  web.on("/screen.bmp", [] {
    if (web.hasArg("page")) page = constrain(web.arg("page").toInt(), 0, PAGES - 1);
    handleBmp();
  });
  web.on("/", [] { web.send(200, "text/plain; charset=utf-8", "額度顯示器運作中。/screen.bmp?page=0~3 取畫面，/update 更新韌體"); });
  web.begin();
  bootScreen(WiFi.status() == WL_CONNECTED ? "向 Mac 取資料" : "Wi-Fi 連不上");
  fetch();
  pageAt = millis(); render();
}

bool pressed(int pin, bool& last) {
  bool now = digitalRead(pin) == LOW, edge = now && !last;
  last = now; return edge;
}

void loop() {
  ArduinoOTA.handle(); web.handleClient();
  static bool k1 = false, k2 = false; static uint32_t lastDraw = 0;
  bool redraw = false;
  if (pressed(PIN_KEY1, k1)) { page = (page + 1) % PAGES; pageAt = millis(); redraw = true; }
  if (pressed(PIN_KEY2, k2)) { autoPlay = !autoPlay; pageAt = millis(); redraw = true; }
  if (autoPlay && millis() - pageAt > PAGE_MS) { page = (page + 1) % PAGES; pageAt = millis(); redraw = true; }
  if (WiFi.status() != WL_CONNECTED && millis() - lastTryMs > 30000) { WiFi.reconnect(); lastTryMs = millis(); }
  if (millis() - lastTryMs > (M.ok ? FETCH_MS : 20000)) { fetch(); redraw = true; }
  if (redraw || millis() - lastDraw > 1000) { render(); lastDraw = millis(); }
  delay(10);
}
