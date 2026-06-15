// Smart Board — Hebrew word clock
//
// Displays the current time in Hebrew words
// on a 7.5" e-ink display (Seeed XIAO e-paper driver, model 502).
//
// Time source : DS3231 hardware RTC (I2C, SDA=D4/GPIO6  SCL=D5/GPIO7)
// Time setting: connect to the open "HebTime" WiFi AP (192.168.4.1 —
//               a captive portal opens the page automatically) and click
//               "Set Time". The browser's current time is written to the
//               RTC as UTC; the Israel timezone (with DST) is applied for
//               display, so the clock stays correct across DST changes.
//
// Update cadence: checks every second, redraws on minute change.
// No deep sleep — the AP stays active for time resets at any time.
//
// SETUP NOTE — partial refresh:
//   To enable partial-refresh updates, add this line to your library
//   setup file (e.g. User_Setups/Setup502_Seeed_XIAO_EPaper_7inch5.h):
//
//     #define USE_PARTIAL_EPAPER
//
//   Without it, epaper.updataPartial() below won't compile.
//   (Yes, "updata" — that's the actual name in the library.)
//
#include "driver.h"
#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <Fonts/Custom/NotoSerifHebrew_Bold_85.h>
#include "time_words.h"
#include "hebrew_date.h"

#ifdef EPAPER_ENABLE

EPaper epaper = EPaper();

// ── AP config ────────────────────────────────────────────────────────
#define AP_SSID     "HebTime"
#define AP_PASSWORD ""         // empty = open network

// ── Timezone (Israel: UTC+2 standard, UTC+3 DST) ─────────────────────
const char* timeZone = "IST-2IDT,M3.4.4/26,M10.5.0";

// ── Hardware instances ────────────────────────────────────────────────
RTC_DS3231  rtc;
WebServer   server(80);
DNSServer   dnsServer;
Preferences prefs;
static const byte DNS_PORT = 53;

// ── Date-display mode (persisted in NVS) ─────────────────────────────
// false = Gregorian date on the top row, true = Hebrew (Jewish) date.
static bool showHebrewDate   = false;
static bool pendingFullRefresh = false;   // set when the date line must be redrawn

// ── Layout ────────────────────────────────────────────────────────────
#define SCREEN_W           800
#define SCREEN_H           480
#define TEXT_SCALE         1
#define FONT_BASE_H        85
#define HEBREW_SPACE_W     20
#define LINE_GAP           24
#define FONT_ASCENT        55
#define FONT_DESCENT        7
#define TIME_BOX_X         0
#define TIME_BOX_Y         60
#define TIME_BOX_W         800
#define TIME_BOX_H         400
#define MAX_LINES          3
#define FULL_REFRESH_EVERY 15

// ── Date line (drawn above the time, in a smaller font) ───────────────
// The Hebrew font is a fixed 85px bitmap, so "smaller" is done by
// downscaling glyphs to NUM/DEN of full size (1/2 ≈ 42px here).
#define DATE_SCALE_NUM     1
#define DATE_SCALE_DEN     2
#define DATE_BASELINE_Y    48     // baseline in the 0..60 strip above the time box

// ── Time font scale (NUM/DEN of the 85px font); 16/17 ≈ 80px ──────────
#define TIME_SCALE_NUM     16
#define TIME_SCALE_DEN     17

// ── Display state ─────────────────────────────────────────────────────
static bool firstDraw     = true;
static int  partialCount  = 0;
static char prevLines[MAX_LINES][64];
static int  prevLineCount = 0;

// ── Runtime state ─────────────────────────────────────────────────────
static bool rtcReady            = false;
static int  lastDisplayedMinute = -1;

// ── Types used by Hebrew RTL + niqud rendering ───────────────────────
struct GlyphMetrics {
  uint8_t  width;
  uint8_t  height;
  uint8_t  xAdvance;
  int8_t   xOffset;
  int8_t   yOffset;
};

// ── Forward declarations ──────────────────────────────────────────────
void applyTimeZone();
void syncSystemTimeFromRTC();
void setupAP();
void handleRoot();
void handleSetTime();
void handleDateMode();
void drawTimeInWords(const struct tm& t, bool fullRefresh);
void drawCenteredLines(const String& l1, const String& l2, const String& l3, int numLines);
void splitTimePhrase(const struct tm& t, String& line1, String& line2, String& line3);
String buildDateString(const struct tm& t);
void drawDateLine(const struct tm& t);
void drawError(const String& msg);

// ──────────────────────────────────────────────────────
//  setup() — entry point on every wake
// ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);

  epaper.begin(0);
  epaper.setRotation(0);
  applyTimeZone();

  // Load persisted settings before the first draw so the top row uses the
  // chosen date mode immediately.
  prefs.begin("hebtime", false);
  showHebrewDate = (prefs.getUChar("datemode", 0) == 1);
  Serial.printf("dateMode = %s\n", showHebrewDate ? "Hebrew" : "Gregorian");

  // XIAO ESP32-C3 I2C on D4/D5. GPIO4/GPIO5 (D2/D3) are NOT free here —
  // the e-paper uses them for BUSY/DC — so the RTC must sit on D4=GPIO6 (SDA)
  // and D5=GPIO7 (SCL).
  Wire.begin(6, 7);
  if (!rtc.begin()) {
    Serial.println("RTC not found — check I2C wiring (SDA=D4/GPIO6 SCL=D5/GPIO7)");
    drawError("RTC not found - check wiring");
    // AP still starts so time can be set once wiring is fixed
  } else {
    Serial.printf("RTC found  lostPower=%d\n", rtc.lostPower());
    if (!rtc.lostPower()) {
      syncSystemTimeFromRTC();
      rtcReady = true;
      struct tm t;
      if (getLocalTime(&t)) {
        drawTimeInWords(t, true);
        firstDraw = false;
        lastDisplayedMinute = t.tm_min;
      }
    } else {
      Serial.println("RTC lost power — time not set, waiting for AP set");
      drawError("Connect to HebTime WiFi to set the time");
    }
  }

  setupAP();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  if (!rtcReady) return;

  // Re-sync system clock from RTC every 60 s to prevent soft-clock drift
  static unsigned long lastRtcSyncMs = 0;
  unsigned long ms = millis();
  if (ms - lastRtcSyncMs >= 60000UL) {
    syncSystemTimeFromRTC();
    lastRtcSyncMs = ms;
  }

  struct tm t;
  if (!getLocalTime(&t)) return;

  if (t.tm_min != lastDisplayedMinute || pendingFullRefresh) {
    drawTimeInWords(t, firstDraw || pendingFullRefresh);
    firstDraw = false;
    pendingFullRefresh = false;
    lastDisplayedMinute = t.tm_min;
  }
}

// ─────────────────────────────────────────────────────────────────────
//  Timezone
// ─────────────────────────────────────────────────────────────────────
void applyTimeZone() {
  setenv("TZ", timeZone, 1);
  tzset();
  Serial.printf("applyTimeZone: TZ=%s\n", timeZone);
}

// ─────────────────────────────────────────────────────────────────────
//  RTC helper — reads UTC from DS3231 and syncs the ESP32 soft-clock
// ─────────────────────────────────────────────────────────────────────
void syncSystemTimeFromRTC() {
  DateTime now = rtc.now();
  time_t utc = (time_t)now.unixtime();
  struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  Serial.printf("syncSystemTimeFromRTC: UTC epoch=%lld\n", (long long)utc);
}

// ─────────────────────────────────────────────────────────────────────
//  Web page (single self-contained HTML, no external resources)
// ─────────────────────────────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HebTime</title>
<style>
body{font-family:sans-serif;text-align:center;padding:40px 20px;
     background:#1a1a2e;color:#eee;margin:0}
h1{font-size:1.8em;margin-bottom:8px}
p{color:#aaa;margin-top:0}
#clock{font-size:2.8em;margin:28px 0 6px;
       font-variant-numeric:tabular-nums;letter-spacing:2px}
#date{font-size:1.1em;color:#aaa;margin-bottom:36px}
button{padding:16px 48px;font-size:1.2em;background:#e94560;
       color:#fff;border:none;border-radius:10px;cursor:pointer}
button:hover,button:active{background:#c73652}
#mode{margin-top:28px;font-size:1.05em;color:#ccc}
#mode .lbl{display:block;margin-bottom:8px;color:#aaa}
#mode label{margin:0 12px;cursor:pointer}
#status{margin-top:24px;font-size:1em;min-height:1.5em}
.ok{color:#a8ff78}.err{color:#ff6b6b}
</style></head><body>
<h1>HebTime</h1>
<p>Tap the button to set the clock to this device's time</p>
<div id="clock">--:--:--</div>
<div id="date"></div>
<button onclick="setTime()">Set Time</button>
<div id="mode">
  <span class="lbl">Top row date</span>
  <label><input type="radio" name="dm" value="0" {{G_CHECKED}} onchange="setMode(0)"> Gregorian</label>
  <label><input type="radio" name="dm" value="1" {{H_CHECKED}} onchange="setMode(1)"> Hebrew</label>
</div>
<div id="status"></div>
<script>
function pad(n){return String(n).padStart(2,'0')}
function tick(){
  const d=new Date();
  document.getElementById('clock').textContent=
    pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());
  document.getElementById('date').textContent=
    d.toLocaleDateString(undefined,
      {weekday:'long',year:'numeric',month:'long',day:'numeric'});
}
setInterval(tick,1000);tick();
function setTime(){
  const epoch=Math.floor(Date.now()/1000);
  const s=document.getElementById('status');
  s.textContent='Setting...';s.className='';
  fetch('/settime',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'epoch='+epoch})
  .then(r=>{if(!r.ok)throw new Error(r.statusText);return r.text();})
  .then(t=>{s.textContent=t;s.className='ok';})
  .catch(e=>{s.textContent='Error: '+e;s.className='err';});
}
function setMode(m){
  const s=document.getElementById('status');
  s.textContent='Saving...';s.className='';
  fetch('/datemode',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'mode='+m})
  .then(r=>{if(!r.ok)throw new Error(r.statusText);return r.text();})
  .then(t=>{s.textContent=t;s.className='ok';})
  .catch(e=>{s.textContent='Error: '+e;s.className='err';});
}
</script></body></html>
)rawliteral";

// ─────────────────────────────────────────────────────────────────────
//  Web handlers
// ─────────────────────────────────────────────────────────────────────
void handleRoot() {
  String page = FPSTR(INDEX_HTML);
  page.replace("{{G_CHECKED}}", showHebrewDate ? "" : "checked");
  page.replace("{{H_CHECKED}}", showHebrewDate ? "checked" : "");
  server.send(200, "text/html", page);
}

// Select Gregorian (mode=0) or Hebrew (mode=1) date for the top row.
void handleDateMode() {
  if (!server.hasArg("mode")) {
    server.send(400, "text/plain", "Missing mode");
    return;
  }
  bool hebrew = (server.arg("mode").toInt() == 1);
  if (hebrew != showHebrewDate) {
    showHebrewDate = hebrew;
    prefs.putUChar("datemode", hebrew ? 1 : 0);
    pendingFullRefresh = true;     // redraw the top row on the next loop
  }
  server.send(200, "text/plain",
              hebrew ? "Showing Hebrew date" : "Showing Gregorian date");
  Serial.printf("dateMode set to %s\n", hebrew ? "Hebrew" : "Gregorian");
}

void handleSetTime() {
  if (!server.hasArg("epoch")) {
    server.send(400, "text/plain", "Missing epoch");
    return;
  }
  time_t epoch = (time_t)server.arg("epoch").toInt();
  if (epoch < 1000000000L) {
    server.send(400, "text/plain", "Invalid epoch");
    return;
  }

  // Write UTC to DS3231
  rtc.adjust(DateTime((uint32_t)epoch));
  // Sync ESP32 soft-clock
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  rtcReady = true;
  lastDisplayedMinute = -1;  // force display redraw on next loop

  struct tm localTm;
  localtime_r(&epoch, &localTm);
  char response[48];
  snprintf(response, sizeof(response), "Time set: %02d:%02d",
           localTm.tm_hour, localTm.tm_min);
  server.send(200, "text/plain", response);
  Serial.printf("RTC adjusted — epoch=%lld  local=%02d:%02d\n",
                (long long)epoch, localTm.tm_hour, localTm.tm_min);
}

// ─────────────────────────────────────────────────────────────────────
//  AP + HTTP server startup
// ─────────────────────────────────────────────────────────────────────
void setupAP() {
  WiFi.mode(WIFI_AP);
  const char* pw = (strlen(AP_PASSWORD) > 0) ? AP_PASSWORD : nullptr;
  WiFi.softAP(AP_SSID, pw);
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("AP started  SSID=%s  IP=%s\n",
                AP_SSID, apIP.toString().c_str());

  // Captive portal: resolve every hostname to us so phones auto-open the page.
  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/settime",  HTTP_POST, handleSetTime);
  server.on("/datemode", HTTP_POST, handleDateMode);
  // Any other URL (incl. OS connectivity-check probes) → redirect to the page,
  // which triggers the "sign in to network" captive-portal prompt.
  server.onNotFound([]() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
  Serial.println("HTTP server started");
}

// ──────────────────────────────────────────────────────
//  Hebrew RTL + niqud support.
//  GFXfont can't position combining marks (niqud) correctly
//  because it lacks OpenType GPOS support. We draw glyphs
//  individually and center each niqud mark on its base letter.
// ──────────────────────────────────────────────────────
bool isNiqudCP(uint16_t cp) {
  return (cp >= 0x05B0 && cp <= 0x05BD) || cp == 0x05BF ||
         cp == 0x05C1 || cp == 0x05C2 || cp == 0x05C7;
}

bool isNikudByte(uint8_t b0, uint8_t b1) {
  uint16_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
  return isNiqudCP(cp);
}

uint16_t decodeUTF8(const String& s, int& pos) {
  uint8_t b0 = (uint8_t)s[pos];
  if ((b0 & 0xE0) == 0xC0 && pos + 1 < (int)s.length()) {
    uint8_t b1 = (uint8_t)s[pos + 1];
    pos += 2;
    return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
  }
  pos++;
  return b0;
}

// Draw a glyph at scale sn/sd (sn==sd → 1:1). When shrinking, each
// destination pixel samples its source footprint and goes black when at
// least half the covered source pixels are set (majority sampling keeps
// thin serif strokes legible).
void drawGlyphBitmap(const GFXfont* font, uint16_t cp, int x, int y, int sn, int sd) {
  if (cp < pgm_read_word(&font->first) || cp > pgm_read_word(&font->last)) return;
  const GFXglyph* glyph = &((const GFXglyph*)pgm_read_ptr(&font->glyph))[cp - pgm_read_word(&font->first)];
  const uint8_t* bitmap = (const uint8_t*)pgm_read_ptr(&font->bitmap);

  uint32_t bo = pgm_read_dword(&glyph->bitmapOffset);
  int w = pgm_read_byte(&glyph->width);
  int h = pgm_read_byte(&glyph->height);

  if (sn == sd) {                                   // fast path: 1:1
    int bit = 0;
    for (int row = 0; row < h; row++)
      for (int col = 0; col < w; col++) {
        if (pgm_read_byte(&bitmap[bo + bit / 8]) & (0x80 >> (bit & 7)))
          epaper.drawPixel(x + col, y + row, TFT_BLACK);
        bit++;
      }
    return;
  }

  int dW = (w * sn) / sd;
  int dH = (h * sn) / sd;
  for (int dy = 0; dy < dH; dy++) {
    int sy0 = (dy * sd) / sn;
    int sy1 = ((dy + 1) * sd) / sn;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    if (sy1 > h) sy1 = h;
    for (int dx = 0; dx < dW; dx++) {
      int sx0 = (dx * sd) / sn;
      int sx1 = ((dx + 1) * sd) / sn;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      if (sx1 > w) sx1 = w;
      int black = 0, total = 0;
      for (int sy = sy0; sy < sy1; sy++)
        for (int sx = sx0; sx < sx1; sx++) {
          int bit = sy * w + sx;
          if (pgm_read_byte(&bitmap[bo + bit / 8]) & (0x80 >> (bit & 7))) black++;
          total++;
        }
      if (total > 0 && black * 2 >= total)
        epaper.drawPixel(x + dx, y + dy, TFT_BLACK);
    }
  }
}

GlyphMetrics getGlyphMetrics(const GFXfont* font, uint16_t cp) {
  GlyphMetrics m = {0, 0, 0, 0, 0};
  if (cp < pgm_read_word(&font->first) || cp > pgm_read_word(&font->last)) return m;
  const GFXglyph* glyph = &((const GFXglyph*)pgm_read_ptr(&font->glyph))[cp - pgm_read_word(&font->first)];
  m.width    = pgm_read_byte(&glyph->width);
  m.height   = pgm_read_byte(&glyph->height);
  m.xAdvance = pgm_read_byte(&glyph->xAdvance);
  m.xOffset  = (int8_t)pgm_read_byte(&glyph->xOffset);
  m.yOffset  = (int8_t)pgm_read_byte(&glyph->yOffset);
  return m;
}

int drawHebrewWord(const GFXfont* font, const String& word, int x, int y, int sn, int sd) {
  int cursor = x;
  int lastBaseX = x;
  int lastBaseXOff = 0;
  int lastBaseW = 0;

  int i = 0;
  while (i < (int)word.length()) {
    uint16_t cp = decodeUTF8(word, i);
    GlyphMetrics m = getGlyphMetrics(font, cp);
    int gW   = ((int)m.width    * sn) / sd;
    int gXo  = ((int)m.xOffset  * sn) / sd;
    int gYo  = ((int)m.yOffset  * sn) / sd;
    int gAdv = ((int)m.xAdvance * sn) / sd;

    if (isNiqudCP(cp)) {
      int markX;
      if (cp == 0x05C1)        // SHIN DOT — right side of letter
        markX = lastBaseX + lastBaseXOff + lastBaseW - gW;
      else if (cp == 0x05C2 || cp == 0x05B9)  // SIN DOT / HOLAM — left side
        markX = lastBaseX + lastBaseXOff;
      else                     // all other niqud — centered
        markX = lastBaseX + lastBaseXOff + lastBaseW / 2 - gW / 2;
      int markY = y + gYo;
      drawGlyphBitmap(font, cp, markX, markY, sn, sd);
    } else {
      drawGlyphBitmap(font, cp, cursor + gXo, y + gYo, sn, sd);
      lastBaseX = cursor;
      lastBaseXOff = gXo;
      lastBaseW = gW;
      cursor += gAdv;
    }
  }
  return cursor - x;
}

int measureHebrewWord(const GFXfont* font, const String& word, int sn, int sd) {
  int width = 0;
  int i = 0;
  while (i < (int)word.length()) {
    uint16_t cp = decodeUTF8(word, i);
    if (!isNiqudCP(cp))
      width += ((int)getGlyphMetrics(font, cp).xAdvance * sn) / sd;
  }
  return width;
}

String reverseHebrew(const String& word) {
  String clusters[32];
  int count = 0;
  int i = 0;
  while (i < (int)word.length() && count < 32) {
    uint8_t c = (uint8_t)word[i];
    if ((c & 0xE0) == 0xC0 && i + 1 < (int)word.length()) {
      clusters[count] = word.substring(i, i + 2);
      i += 2;
      while (i + 1 < (int)word.length() &&
             isNikudByte((uint8_t)word[i], (uint8_t)word[i + 1])) {
        clusters[count] += word.substring(i, i + 2);
        i += 2;
      }
      count++;
    } else if (c >= '0' && c <= '9') {
      // Numbers stay left-to-right inside RTL text — keep the whole digit
      // run as one cluster so "14" doesn't get flipped to "41".
      int ds = i;
      while (i < (int)word.length() &&
             (uint8_t)word[i] >= '0' && (uint8_t)word[i] <= '9') i++;
      clusters[count++] = word.substring(ds, i);
    } else {
      clusters[count++] = word.substring(i, i + 1);
      i++;
    }
  }
  String result;
  for (int j = count - 1; j >= 0; j--)
    result += clusters[j];
  return result;
}

// ──────────────────────────────────────────────────────
//  Draw a single line of Hebrew text centred at cx, top at y.
//  Each word is reversed for LTR rendering, and words are
//  placed right-to-left across the line.
// ──────────────────────────────────────────────────────
int drawHebrewLine(const String& text, int cx, int y, int sn, int sd) {
  if (text.length() == 0) return 0;

  const GFXfont* font = &NotoSerifHebrew_Bold_85;
  int spaceW = (HEBREW_SPACE_W * sn) / sd;

  String words[10];
  int wordCount = 0;
  int start = 0;
  for (int i = 0; i <= (int)text.length(); i++) {
    if (i == (int)text.length() || text[i] == ' ') {
      if (i > start && wordCount < 10)
        words[wordCount++] = text.substring(start, i);
      start = i + 1;
    }
  }

  String reversed[10];
  for (int i = 0; i < wordCount; i++)
    reversed[i] = reverseHebrew(words[i]);

  int totalW = 0;
  for (int i = 0; i < wordCount; i++) {
    totalW += measureHebrewWord(font, reversed[i], sn, sd);
    if (i < wordCount - 1) totalW += spaceW;
  }

  int curX = cx - totalW / 2;

  for (int i = wordCount - 1; i >= 0; i--) {
    int wordW = drawHebrewWord(font, reversed[i], curX, y, sn, sd);
    curX += wordW;
    if (i > 0) curX += spaceW;
  }

  return (FONT_BASE_H * sn) / sd;
}

// ──────────────────────────────────────────────────────
//  Draw lines vertically centred inside the TIME_BOX.
// ──────────────────────────────────────────────────────
void drawCenteredLines(const String& l1, const String& l2, const String& l3, int numLines) {
  const int sn = TIME_SCALE_NUM, sd = TIME_SCALE_DEN;
  int cx      = SCREEN_W / 2;
  int fontH   = (FONT_BASE_H  * sn) / sd;
  int ascent  = (FONT_ASCENT  * sn) / sd;
  int descent = (FONT_DESCENT * sn) / sd;
  int lineGap = (LINE_GAP     * sn) / sd;
  int step    = fontH + lineGap;
  int visH    = ascent + (numLines - 1) * step + descent;
  int y       = TIME_BOX_Y + (TIME_BOX_H - visH) / 2 + ascent;

  drawHebrewLine(l1, cx, y, sn, sd);
  drawHebrewLine(l2, cx, y + step, sn, sd);
  if (numLines == 3)
    drawHebrewLine(l3, cx, y + 2 * step, sn, sd);
}

// ──────────────────────────────────────────────────────
//  Build the two-line phrase for the current time
// ──────────────────────────────────────────────────────
static int countWords(const String& s) {
  int n = 0;
  bool inWord = false;
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == ' ') { inWord = false; }
    else if (!inWord) { inWord = true; n++; }
  }
  return n;
}

void splitTimePhrase(const struct tm& t, String& line1, String& line2, String& line3) {
  int hour12 = t.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  int min = t.tm_min;
  String period = String(getTimePeriod(t.tm_hour));
  line3 = "";

  if (isSubtractMinute(min)) {
    int next = (hour12 % 12) + 1;       // 12 -> 1
    line1 = String(SUBTRACT_AMOUNT[min]) + " " + String(HOURS_LAMED[next - 1]);
    line2 = period;
  } else if (min == 0) {
    line1 = String(HOURS[hour12 - 1]);
    line2 = period;
  } else {
    String minPart = String(MINUTE_PREFIX[min]);
    if (hour12 >= 11 || countWords(minPart) == 3) {
      line1 = String(HOURS[hour12 - 1]);
      line2 = minPart;
      line3 = period;
    } else {
      line1 = String(HOURS[hour12 - 1]) + " " + minPart;
      line2 = period;
    }
  }
}

// ──────────────────────────────────────────────────────
//  Date line — "<day> <hebrew-month> <year>", e.g. "14 בְּיוּנִי 2026".
//  Drawn in a smaller font above the time. Hebrew reads right-to-left,
//  so the day ends up on the right and the year on the left.
// ──────────────────────────────────────────────────────
String buildDateString(const struct tm& t) {
  int day      = t.tm_mday;
  int monthIdx = t.tm_mon + 1;          // tm_mon is 0..11
  int year     = t.tm_year + 1900;
  String month = (monthIdx >= 1 && monthIdx <= 12)
                   ? String(SUBTRACT_MONTH[monthIdx]) : String("");
  return String(day) + " " + month + " " + String(year);
}

void drawDateLine(const struct tm& t) {
  String dateStr;
  if (showHebrewDate) {
    const char* heb = hebrewDateForTm(t);          // from hebrew_date.h
    if (heb) {
      dateStr = String(heb);
      // The font ends at 0x05EA, so it has no glyphs for the Hebrew
      // geresh/gershayim numeral punctuation (U+05F3 ׳ / U+05F4 ״). Swap in
      // the ASCII apostrophe/quote, which DO exist in the font (0x27 / 0x22),
      // sit at the same height, and read identically. The UTF-8 byte pairs
      // are D7 B3 (geresh) and D7 B4 (gershayim).
      dateStr.replace("\xD7\xB4", "\"");   // gershayim ״ → "
      dateStr.replace("\xD7\xB3", "'");    // geresh    ׳ → '
    } else {
      // Fall back to the Gregorian date if t is outside the generated range.
      dateStr = buildDateString(t);
    }
  } else {
    dateStr = buildDateString(t);
  }
  drawHebrewLine(dateStr, SCREEN_W / 2, DATE_BASELINE_Y,
                 DATE_SCALE_NUM, DATE_SCALE_DEN);
}

// ──────────────────────────────────────────────────────
//  Main draw routine
//  fullRefresh=true on first boot only — clears the whole
//  screen and uses full e-ink update. Subsequent calls
//  redraw only the time box and use partial refresh.
// ──────────────────────────────────────────────────────
void drawTimeInWords(const struct tm& t, bool fullRefresh) {
  String line1, line2, line3;
  splitTimePhrase(t, line1, line2, line3);

  int numLines = (line3.length() > 0) ? 3 : 2;

  static int lastDrawnYday = -1;
  bool dateChanged = (t.tm_yday != lastDrawnYday);

  // Check if anything actually changed since last draw
  if (!fullRefresh && !dateChanged) {
    bool anyChanged = (numLines != prevLineCount);
    if (!anyChanged) {
      for (int i = 0; i < numLines; i++) {
        const char* prev = prevLines[i];
        const char* cur  = (i == 0) ? line1.c_str() : (i == 1) ? line2.c_str() : line3.c_str();
        if (strcmp(cur, prev) != 0) {
          anyChanged = true;
          break;
        }
      }
    }
    if (!anyChanged) {
      Serial.println("No lines changed — skipping refresh");
      return;
    }
  }

  Serial.printf("Drawing time hour=%d min=%d fullRefresh=%d\n",
    t.tm_hour, t.tm_min, fullRefresh);

  bool doFullRefresh = fullRefresh || dateChanged || (partialCount >= FULL_REFRESH_EVERY);

  // For differential partial refresh: render old text first to build
  // the pixel-perfect old buffer the UC8179 needs for clean transitions.
  uint8_t* oldBuf = nullptr;
  if (!doFullRefresh && prevLineCount > 0) {
    epaper.fillRect(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H, TFT_WHITE);
    drawCenteredLines(String(prevLines[0]), String(prevLines[1]),
                      String(prevLines[2]), prevLineCount);
    oldBuf = epaper.capturePartialWindow(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H);
  }

  // Render new text. A full refresh clears the whole panel, so the date
  // (which lives above the time box) must be redrawn then. Partial updates
  // only touch the time box, leaving the date intact from the last full draw.
  if (doFullRefresh) {
    epaper.fillScreen(TFT_WHITE);
    drawDateLine(t);
  } else {
    epaper.fillRect(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H, TFT_WHITE);
  }

  drawCenteredLines(line1, line2, line3, numLines);

  if (doFullRefresh) {
    epaper.update();
    partialCount = 0;
  } else {
    epaper.updataPartial(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H, oldBuf);
    partialCount++;
  }
  if (oldBuf) free(oldBuf);

  // Save current state for next partial-refresh comparison
  for (int i = 0; i < MAX_LINES; i++) {
    if (i < numLines) {
      const char* src = (i == 0) ? line1.c_str() : (i == 1) ? line2.c_str() : line3.c_str();
      strncpy(prevLines[i], src, sizeof(prevLines[i]) - 1);
    } else {
      prevLines[i][0] = '\0';
    }
  }
  prevLineCount = numLines;
  lastDrawnYday = t.tm_yday;

  Serial.printf("Drew \"%s\" / \"%s\" / \"%s\"\n", line1.c_str(), line2.c_str(), line3.c_str());
}

// ──────────────────────────────────────────────────────
//  Error screen
// ──────────────────────────────────────────────────────
void drawError(const String& msg) {
  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_BLACK);
  epaper.drawCentreString("Error:",      SCREEN_W/2, SCREEN_H/2 - 30, 4);
  epaper.drawCentreString(msg.c_str(),   SCREEN_W/2, SCREEN_H/2 + 10, 2);
  epaper.update();
}

#else
// Stubs when EPAPER_ENABLE isn't defined (mirrors the original sketch's behaviour).
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("EPAPER: NOT DEFINED");
}
void loop() {}
#endif
