#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

// ---------- CHANGE THESE ----------
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* HOSTNAME      = "ledmatrix";

// Fixed LAN address, so the router's port forward always points at the ESP32.
// Pick a free address on your network, outside your router's DHCP range.
#define USE_STATIC_IP 1               // 0 = get an address from the router (DHCP)
const IPAddress STATIC_IP (192, 168, 1, 50);
const IPAddress GATEWAY   (192, 168, 1, 1);     // your router
const IPAddress SUBNET    (255, 255, 255, 0);
const IPAddress DNS_SERVER(8, 8, 8, 8);
const int   TCP_PORT      = 8443;            // forward this port on your router to the ESP32
// 64 hex characters (32 bytes). Make one with:  openssl rand -hex 32
// Must be identical to DEVICE_KEY on Vercel. It both authenticates and encrypts.
const char* DEVICE_KEY    = "PASTE-64-HEX-CHARACTERS-HERE";
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

WiFiServer tcpServer(TCP_PORT);
WiFiClient client;
bool clientActive = false;
unsigned long clientSince = 0;
uint8_t key[32];
uint8_t challenge[16];
const size_t MAX_FRAME = 256;
uint8_t rx[2 + MAX_FRAME];
size_t rxLen = 0;

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

// ---------- Incoming messages (encrypted TCP) ----------
//
// Protocol (the Vercel site is the client):
//   ESP32 -> site : 16 random bytes (challenge, new for every connection)
//   site -> ESP32 : [2-byte length][12-byte IV | AES-256-GCM ciphertext | 16-byte tag]
//                   key = DEVICE_KEY, AAD = challenge  (so recorded requests can't be replayed)
//   ESP32 -> site : same framing, AAD = challenge + "R"
// Requests: "M\n<RRGGBB>\n<song>\n<text>" shows a message, "S" asks for the current status.

bool parseKey() {
  if (strlen(DEVICE_KEY) != 64) return false;
  for (int i = 0; i < 32; i++) {
    char h[3] = { DEVICE_KEY[2 * i], DEVICE_KEY[2 * i + 1], 0 };
    char* end;
    key[i] = (uint8_t)strtol(h, &end, 16);
    if (*end) return false;
  }
  return true;
}

void closeClient() {
  client.stop();
  clientActive = false;
  rxLen = 0;
}

void authFailed() {
  if (++authFailures >= 5) { lockedUntil = millis() + 60000; authFailures = 0; }   // 5 bad tries: ignore everyone for 1 min
  closeClient();
}

// Returns the reply text for a decrypted request.
String handleRequest(const String& req) {
  if (req == "S") return currentColorHex + "\n" + currentText;

  if (req.startsWith("M\n") && req.length() > 9 && req[8] == '\n') {
    String hex = req.substring(2, 8);
    int b = req.indexOf('\n', 9);
    if (b < 0) return "ERR";
    int song = req.substring(9, b).toInt();
    String text = req.substring(b + 1);

    long rgb = strtol(hex.c_str(), nullptr, 16);
    uint16_t color = matrix.Color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    if (!setText(text, color)) return "ERR";
    currentColorHex = hex;
    startSong(song);
    return "OK";
  }
  return "ERR";
}

void sendReply(const String& reply) {
  uint8_t out[2 + 12 + 160 + 16];
  size_t n = min((size_t)reply.length(), (size_t)160);
  uint8_t aad[17];
  memcpy(aad, challenge, 16);
  aad[16] = 'R';

  uint8_t* frame = out + 2;
  esp_fill_random(frame, 12);                                   // fresh IV
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
  mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, frame, 12, aad, sizeof(aad),
                            (const uint8_t*)reply.c_str(), frame + 12, 16, frame + 12 + n);
  mbedtls_gcm_free(&g);

  size_t frameLen = 12 + n + 16;
  out[0] = frameLen >> 8;
  out[1] = frameLen & 0xFF;
  client.write(out, 2 + frameLen);
  client.flush();
}

void processFrame(const uint8_t* f, size_t n) {
  size_t ptLen = n - 12 - 16;
  uint8_t pt[MAX_FRAME];
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
  int ret = mbedtls_gcm_auth_decrypt(&g, ptLen, f, 12, challenge, sizeof(challenge),
                                     f + n - 16, 16, f + 12, pt);
  mbedtls_gcm_free(&g);
  if (ret != 0) { authFailed(); return; }                       // wrong key or tampered/replayed

  authFailures = 0;
  pt[ptLen] = 0;
  sendReply(handleRequest(String((const char*)pt)));
  closeClient();
}

// Called every loop(); never blocks.
void serviceNetwork() {
  WiFiClient incoming = tcpServer.available();
  if (incoming) {
    if (millis() < lockedUntil) {
      incoming.stop();
    } else {
      if (clientActive) closeClient();                          // newest connection wins
      client = incoming;
      client.setNoDelay(true);
      esp_fill_random(challenge, sizeof(challenge));
      client.write(challenge, sizeof(challenge));
      clientActive = true;
      clientSince = millis();
      rxLen = 0;
    }
  }
  if (!clientActive) return;

  if (millis() - clientSince > 5000 || !client.connected()) { closeClient(); return; }

  while (client.available() && rxLen < sizeof(rx)) rx[rxLen++] = client.read();
  if (rxLen < 2) return;

  size_t n = ((size_t)rx[0] << 8) | rx[1];
  if (n < 12 + 16 + 1 || n > MAX_FRAME - 1) { authFailed(); return; }   // malformed
  if (rxLen >= 2 + n) processFrame(rx + 2, n);
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
#if USE_STATIC_IP
  if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS_SERVER)) Serial.println("Static IP config failed");
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  if (!parseKey()) {
    Serial.println("DEVICE_KEY must be exactly 64 hex characters");
    setText("BAD KEY", matrix.Color(255, 0, 0));
    while (true) delay(1000);
  }
}

void loop() {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected && !wasConnected) {
    Serial.println("Connected, LAN IP " + WiFi.localIP().toString() + ", port " + String(TCP_PORT));
    setText("Online", DEFAULT_COLOR());
    currentColorHex = "00c8ff";
    if (!serverStarted) {        // start only once WiFi is up
      tcpServer.begin();
      serverStarted = true;
    }
  }
  wasConnected = connected;

  if (serverStarted) serviceNetwork();
  updateScrolling();
  updateSong();
}
