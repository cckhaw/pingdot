#include <Arduino.h>
#include <WiFi.h>
#include <HTTPSServer.hpp>      // library: "ESP32 HTTPS Server" by Frank Hessel
#include <SSLCert.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <ResourceNode.hpp>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include "cert.h"                // made by tools/gen-cert.sh (certificate + private key)

using namespace httpsserver;

// ---------- CHANGE THESE ----------
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* HOSTNAME      = "ledmatrix";
const int   HTTPS_PORT    = 8443;            // forward this port on your router to the ESP32
const char* DEVICE_SECRET = "CHANGE-ME-LONG-RANDOM-STRING";   // must equal DEVICE_SECRET on Vercel
// ----------------------------------

#define LED_PIN       16
#define BUZZER_PIN    17
#define USE_BUZZER    1      // 1 = play sounds when new text arrives, 0 = silent
#define MATRIX_W      32     // two 16x16 panels side by side
#define MAX_TEXT_LEN  100

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  16, 16, 2, 1, LED_PIN,
  NEO_MATRIX_BOTTOM     + NEO_MATRIX_RIGHT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG +
  NEO_TILE_BOTTOM       + NEO_TILE_RIGHT +
  NEO_TILE_ROWS      + NEO_TILE_PROGRESSIVE,
  NEO_GRB + NEO_KHZ800
);

String currentText = "Starting...";
int textWidth = 0;
int xPos = 0;
bool scrolling = false;
unsigned long lastScroll = 0;
const int scrollInterval = 60;   // ms per pixel step (lower = faster)
bool wasConnected = false;
bool serverStarted = false;
String currentColorHex = "00c8ff";
int authFailures = 0;
unsigned long lockedUntil = 0;

SSLCert cert = SSLCert(cert_der, cert_der_len, key_der, key_der_len);
HTTPSServer secureServer = HTTPSServer(&cert, HTTPS_PORT, 2);   // max 2 connections

// ---------- Songs (non-blocking; order must match the website's dropdown) ----------

struct Note { uint16_t freq; uint16_t ms; };   // freq 0 = rest

const Note SONG_BEEP[]     = { {1200, 80} };
const Note SONG_DINGDONG[] = { {659, 450}, {523, 700} };
const Note SONG_TWINKLE[]  = {
  {523,300},{523,300},{784,300},{784,300},{880,300},{880,300},{784,600},
  {698,300},{698,300},{659,300},{659,300},{587,300},{587,300},{523,600} };
const Note SONG_ODE[]      = {
  {659,300},{659,300},{698,300},{784,300},{784,300},{698,300},{659,300},{587,300},
  {523,300},{523,300},{587,300},{659,300},{659,450},{587,150},{587,600} };
const Note SONG_BIRTHDAY[] = {
  {784,300},{784,150},{880,450},{784,450},{1047,450},{988,900},
  {784,300},{784,150},{880,450},{784,450},{1175,450},{1047,900} };

struct Song { const Note* notes; int len; };
#define SONG(a) { a, (int)(sizeof(a) / sizeof(a[0])) }
const Song SONGS[] = {          // index 0 = none
  { nullptr, 0 },
  SONG(SONG_BEEP), SONG(SONG_DINGDONG), SONG(SONG_TWINKLE), SONG(SONG_ODE), SONG(SONG_BIRTHDAY)
};
const int SONG_COUNT = sizeof(SONGS) / sizeof(SONGS[0]);

const Song* playing = nullptr;
int noteIdx = 0;
bool inGap = false;
unsigned long nextNoteAt = 0;

void startSong(int id) {
#if USE_BUZZER
  if (id <= 0 || id >= SONG_COUNT) return;
  playing = &SONGS[id];
  noteIdx = 0;
  inGap = false;
  nextNoteAt = 0;
#endif
}

void updateSong() {
#if USE_BUZZER
  if (!playing || millis() < nextNoteAt) return;
  if (inGap) {                   // short silence between notes so repeats are audible
    inGap = false;
    if (noteIdx >= playing->len) { playing = nullptr; return; }
  } else if (noteIdx > 0) {
    noTone(BUZZER_PIN);
    inGap = true;
    nextNoteAt = millis() + 25;
    return;
  }
  if (noteIdx >= playing->len) { noTone(BUZZER_PIN); playing = nullptr; return; }
  Note n = playing->notes[noteIdx++];
  if (n.freq) tone(BUZZER_PIN, n.freq); else noTone(BUZZER_PIN);
  nextNoteAt = millis() + n.ms;
#endif
}

// ---------- Display ----------

void drawText() {
  matrix.fillScreen(0);
  matrix.setCursor(xPos, 4);     // y=4 centers 8px text on 16 rows
  matrix.print(currentText);
  matrix.show();
}

// Sets new text: static + centered if it fits, otherwise scrolls and loops.
bool setText(String t, uint16_t color) {
  String clean = "";
  for (unsigned int i = 0; i < t.length() && clean.length() < MAX_TEXT_LEN; i++) {
    char c = t[i];
    clean += (c >= 32 && c <= 126) ? c : '?';
  }
  clean.trim();
  if (clean.length() == 0) return false;

  currentText = clean;
  matrix.setTextColor(color);
  textWidth = currentText.length() * 6 - 1;   // 5px glyph + 1px spacing
  scrolling = textWidth > MATRIX_W;
  xPos = scrolling ? MATRIX_W : (MATRIX_W - textWidth) / 2;
  drawText();
  return true;
}

uint16_t DEFAULT_COLOR() { return matrix.Color(0, 200, 255); }

void updateScrolling() {
  if (!scrolling) return;
  if (millis() - lastScroll < scrollInterval) return;
  lastScroll = millis();

  drawText();
  xPos--;
  if (xPos < -textWidth) {
    xPos = MATRIX_W;             // loop: start again from the right edge
  }
}

// ---------- Incoming requests (HTTPS, from the Vercel site) ----------

bool secretMatches(const std::string& given) {
  size_t n = strlen(DEVICE_SECRET);
  uint8_t diff = (given.size() != n);
  for (size_t i = 0; i < n; i++) diff |= (i < given.size() ? given[i] : 0) ^ DEVICE_SECRET[i];
  return diff == 0;
}

// Checks the X-Auth header. 5 wrong tries in a row lock requests out for a minute.
bool authorized(HTTPRequest* req, HTTPResponse* res) {
  if (millis() < lockedUntil) {
    res->setStatusCode(429);
    res->print("locked");
    return false;
  }
  if (!secretMatches(req->getHeader("X-Auth"))) {
    if (++authFailures >= 5) { lockedUntil = millis() + 60000; authFailures = 0; }
    res->setStatusCode(401);
    res->print("unauthorized");
    return false;
  }
  authFailures = 0;
  return true;
}

// POST /message, body "<RRGGBB>\n<song>\n<text>"
void handleMessage(HTTPRequest* req, HTTPResponse* res) {
  if (!authorized(req, res)) return;

  char buf[256];
  size_t len = 0;
  while (!req->requestComplete() && len < sizeof(buf) - 1) {
    len += req->readBytes((byte*)buf + len, sizeof(buf) - 1 - len);
  }
  if (!req->requestComplete()) { res->setStatusCode(413); res->print("too big"); return; }
  buf[len] = 0;

  String body(buf);
  int a = body.indexOf('\n');
  int b = body.indexOf('\n', a + 1);
  if (a != 6 || b < 0) { res->setStatusCode(400); res->print("bad format"); return; }

  String hex  = body.substring(0, a);
  int    song = body.substring(a + 1, b).toInt();
  String text = body.substring(b + 1);

  long rgb = strtol(hex.c_str(), nullptr, 16);
  uint16_t color = matrix.Color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
  if (!setText(text, color)) { res->setStatusCode(400); res->print("empty text"); return; }
  currentColorHex = hex;
  startSong(song);
  res->print("ok");
}

// GET /status -> "<RRGGBB>\n<current text>"  (also how the site checks we're online)
void handleStatus(HTTPRequest* req, HTTPResponse* res) {
  if (!authorized(req, res)) return;
  res->setHeader("Content-Type", "text/plain");
  res->print(currentColorHex.c_str());
  res->print("\n");
  res->print(currentText.c_str());
}

// ---------- Setup / loop ----------

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);

  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(5);      // keep low; use an external 5V supply for more
  setText("WiFi...", DEFAULT_COLOR());

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected && !wasConnected) {
    Serial.println("Connected, LAN IP " + WiFi.localIP().toString() + ", HTTPS port " + String(HTTPS_PORT));
    setText("Online", DEFAULT_COLOR());
    currentColorHex = "00c8ff";
    if (!serverStarted) {        // start only once WiFi is up
      secureServer.registerNode(new ResourceNode("/message", "POST", &handleMessage));
      secureServer.registerNode(new ResourceNode("/status", "GET", &handleStatus));
      secureServer.start();
      serverStarted = true;
    }
  }
  wasConnected = connected;

  if (serverStarted) secureServer.loop();
  updateScrolling();
  updateSong();
}
