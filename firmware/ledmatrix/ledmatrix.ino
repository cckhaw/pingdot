#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

// ---------- CHANGE THESE ----------
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* HOSTNAME      = "ledmatrix";
const char* MESSAGE_URL   = "https://YOUR-APP.vercel.app/api/latest";
// ----------------------------------

#define LED_PIN       16
#define BUZZER_PIN    17
#define USE_BUZZER    1      // 1 = play sounds when new text arrives, 0 = silent
#define MATRIX_W      32     // two 16x16 panels side by side
#define MAX_TEXT_LEN  100

// Website is polled every POLL_INTERVAL ms; every HEARTBEAT_EVERY-th poll also
// tells the site "I'm online" (keeps the free Redis command count low).
const unsigned long POLL_INTERVAL = 8000;
const int HEARTBEAT_EVERY = 4;

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
unsigned long lastPoll = 0;
bool polledOnce = false;
int pollCount = 0;
String lastId = "";
bool firstFetchDone = false;

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

// ---------- Fetch from website ----------

// Endpoint returns "<id>\n<RRGGBB>\n<song>\n<text>". Shows it when the id changes.
void pollMessage() {
  String url = MESSAGE_URL;
  if (pollCount % HEARTBEAT_EVERY == 0) url += "?hb=1";
  pollCount++;

  WiFiClientSecure client;
  client.setInsecure();          // skips certificate check; fine for a public read-only message
  HTTPClient http;
  http.setTimeout(4000);
  if (!http.begin(client, url)) return;

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    int a = body.indexOf('\n');
    int b = body.indexOf('\n', a + 1);
    int c = body.indexOf('\n', b + 1);
    if (a > 0 && b > a && c > b) {
      String id    = body.substring(0, a);
      String hex   = body.substring(a + 1, b);
      int    song  = body.substring(b + 1, c).toInt();
      String text  = body.substring(c + 1);
      if (id != lastId && id != "0") {
        long rgb = strtol(hex.c_str(), nullptr, 16);
        uint16_t color = matrix.Color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        if (setText(text, color) && firstFetchDone) startSong(song);   // no sound for the message already there at boot
        lastId = id;
      }
    }
    firstFetchDone = true;
  } else {
    Serial.printf("Fetch failed: %d\n", code);
  }
  http.end();
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
    Serial.println("Connected, IP " + WiFi.localIP().toString());
    setText("Online", DEFAULT_COLOR());
    polledOnce = false;          // fetch immediately
  }
  wasConnected = connected;

  if (connected && (!polledOnce || millis() - lastPoll >= POLL_INTERVAL)) {
    polledOnce = true;
    lastPoll = millis();
    pollMessage();
  }

  updateScrolling();
  updateSong();
}
