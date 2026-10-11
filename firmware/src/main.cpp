#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include "secrets.h"

const char* WIFI_SSID     = WIFI_SSID_VALUE;
const char* WIFI_PASSWORD = WIFI_PASSWORD_VALUE;
const char* HOSTNAME      = "ledmatrix";

#define LED_PIN       16
#define BUZZER_PIN    17
#define USE_BUZZER    1      // 1 = short beep when new text arrives, 0 = off
#define MATRIX_W      32     // two 16x16 panels side by side
#define MAX_TEXT_LEN  100

const unsigned long POLL_INTERVAL = 4000;   // ms between checks of the website

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
String lastId = "";
bool firstFetchDone = false;

// ---------- Display ----------

void drawText() {
  matrix.fillScreen(0);
  matrix.setCursor(xPos, 4);     // y=4 centers 8px text on 16 rows
  matrix.print(currentText);
  matrix.show();
}

// Sets new text: static + centered if it fits, otherwise scrolls and loops.
bool setText(String t) {
  String clean = "";
  for (unsigned int i = 0; i < t.length() && clean.length() < MAX_TEXT_LEN; i++) {
    char c = t[i];
    clean += (c >= 32 && c <= 126) ? c : '?';
  }
  clean.trim();
  if (clean.length() == 0) return false;

  currentText = clean;
  textWidth = currentText.length() * 6 - 1;   // 5px glyph + 1px spacing
  scrolling = textWidth > MATRIX_W;
  xPos = scrolling ? MATRIX_W : (MATRIX_W - textWidth) / 2;
  drawText();
  return true;
}

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

// Endpoint returns "<id>\n<text>". Shows the text when the id changes.
void pollMessage() {
  WiFiClientSecure client;
  client.setInsecure();          // no cert pinning; fine for a public, read-only message
  HTTPClient http;
  http.setTimeout(4000);
  if (!http.begin(client, MESSAGE_URL)) return;

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    int nl = body.indexOf('\n');
    if (nl > 0) {
      String id = body.substring(0, nl);
      String text = body.substring(nl + 1);
      if (id != lastId && id != "0") {
        if (setText(text)) {
#if USE_BUZZER
          if (firstFetchDone) tone(BUZZER_PIN, 1200, 80);
#endif
        }
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
  matrix.setTextColor(matrix.Color(0, 200, 255));
  setText("WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected && !wasConnected) {
    Serial.println("Connected, IP " + WiFi.localIP().toString());
    setText("Online");
    lastPoll = 0;                // fetch immediately
  }
  wasConnected = connected;

  if (connected && (lastPoll == 0 || millis() - lastPoll >= POLL_INTERVAL)) {
    lastPoll = millis();
    if (lastPoll == 0) lastPoll = 1;
    pollMessage();
  }

  updateScrolling();
}
