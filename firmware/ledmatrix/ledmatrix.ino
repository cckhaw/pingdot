#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

// ---------- CHANGE THESE ----------
// WiFi name, password and IP are NOT set here: they are entered on the ESP32's setup page
// (see "Network setup" in the README) and stored on the board.
const char* HOSTNAME      = "ledmatrix";
const int   TCP_PORT      = 8443;            // forward this port on your router to the ESP32
// 64 hex characters (32 bytes). Make one with:  openssl rand -hex 32
// Must be identical to DEVICE_KEY on Vercel. It both authenticates and encrypts.
const char* DEVICE_KEY    = "PASTE-64-HEX-CHARACTERS-HERE";
// Setup hotspot, shown when the board has no WiFi settings yet or can't join (password: 8+ characters)
const char* SETUP_AP_NAME     = "LEDMatrix-Setup";
const char* SETUP_AP_PASSWORD = "ledmatrix";
// ----------------------------------

#define BOOT_PIN      0      // hold this button ~3s while running to reopen the setup page

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
String statusText = "Starting...";   // what the website shows as "now showing" (UTF-8)
uint16_t* bmp = nullptr;             // picture sent by the website (RGB565, row-major, bmpW x 16)
int bmpW = 0;
bool bitmapMode = false;
int authFailures = 0;
unsigned long lockedUntil = 0;

WiFiServer tcpServer(TCP_PORT);
WiFiClient client;
bool clientActive = false;
unsigned long clientSince = 0;
uint8_t key[32];
uint8_t challenge[16];
const int MAX_BITMAP_W = 800;                         // pixels; keep equal to MAX_BITMAP_W on the website
const size_t MAX_FRAME = 12 + 9 + 400 + (size_t)MAX_BITMAP_W * 16 * 2 + 16;
uint8_t rx[2 + MAX_FRAME];
size_t rxLen = 0;

// ---------- Songs (non-blocking; order must match the website's dropdown) ----------

struct Note { uint16_t freq; uint16_t ms; };   // freq 0 = rest

const Note SONG_BEEP[]     = { {2093, 80} }; // C7
const Note SONG_DINGDONG[] = { {1318, 450}, {1046, 700} }; // E6, C6
const Note SONG_TWINKLE[]  = {
  {1046,300},{1046,300},{1568,300},{1568,300},{1760,300},{1760,300},{1568,600},
  {1397,300},{1397,300},{1318,300},{1318,300},{1175,300},{1175,300},{1046,600} };

// Super Mario Bros. Main Theme (Intro)
const Note SONG_MARIO[] = {
  {2637, 100}, {0, 50}, {2637, 100}, {0, 150}, {2637, 100}, {0, 150}, {2093, 100}, {2637, 100}, {0, 150}, {3136, 200}, {0, 300}, {1568, 200}
};

// Star Wars - Imperial March
const Note SONG_IMPERIAL[] = {
  {880, 500}, {880, 500}, {880, 500}, {698, 350}, {1046, 150}, {880, 500}, {698, 350}, {1046, 150}, {880, 1000}
};

// Pac-Man Game Start Intro
const Note SONG_PACMAN[] = {
  {988, 120}, {1975, 120}, {1480, 120}, {1245, 120}, {1975, 60}, {1480, 120}, {1245, 240},
  {1046, 120}, {2093, 120}, {1568, 120}, {1318, 120}, {2093, 60}, {1568, 120}, {1318, 240}
};

// Retro Game "Level Up" Sound Effect
const Note SONG_LEVEL_UP[] = {
  {1046, 100}, {1318, 100}, {1568, 100}, {2093, 300}
};

struct Song { const Note* notes; int len; };
#define SONG(a) { a, (int)(sizeof(a) / sizeof(a[0])) }
const Song SONGS[] = {          // index 0 = none
  { nullptr, 0 },
  SONG(SONG_BEEP), SONG(SONG_DINGDONG), SONG(SONG_TWINKLE), SONG(SONG_MARIO),
  SONG(SONG_IMPERIAL), SONG(SONG_PACMAN), SONG(SONG_LEVEL_UP)
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
  if (bitmapMode) {
    for (int x = 0; x < MATRIX_W; x++) {              // only the visible columns
      int sx = x - xPos;
      if (sx < 0 || sx >= bmpW) continue;
      for (int y = 0; y < 16; y++) matrix.drawPixel(x, y, bmp[y * bmpW + sx]);
    }
  } else {
    matrix.setCursor(xPos, 4);   // y=4 centers 8px text on 16 rows
    matrix.print(currentText);
  }
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
  statusText = clean;
  bitmapMode = false;
  free(bmp);
  bmp = nullptr;
  matrix.setTextColor(color);
  textWidth = currentText.length() * 6 - 1;   // 5px glyph + 1px spacing
  scrolling = textWidth > MATRIX_W;
  xPos = scrolling ? MATRIX_W : (MATRIX_W - textWidth) / 2;
  drawText();
  return true;
}

// Shows a picture (big-endian RGB565, row-major, w x 16): static + centered if it fits, else scrolls.
bool setBitmap(const uint8_t* px, int w) {
  uint16_t* nb = (uint16_t*)malloc((size_t)w * 16 * sizeof(uint16_t));
  if (!nb) return false;
  for (int i = 0; i < w * 16; i++) nb[i] = ((uint16_t)px[2 * i] << 8) | px[2 * i + 1];
  free(bmp);
  bmp = nb;
  bmpW = w;
  bitmapMode = true;
  textWidth = w;
  scrolling = w > MATRIX_W;
  xPos = scrolling ? MATRIX_W : (MATRIX_W - w) / 2;
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
// Requests: "S" = status, or a binary "B" message:
//   'B' | song(1) | width(2, BE) | textLen(2, BE) | R G B (3) | text (UTF-8) | pixels (width*16*2)
// The website draws the text itself (any language, emoji) and sends it as a picture.

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
String handleRequest(const uint8_t* p, size_t len) {
  if (len == 1 && p[0] == 'S') return currentColorHex + "\n" + statusText;

  if (len >= 9 && p[0] == 'B') {
    int song = p[1];
    int w = (p[2] << 8) | p[3];
    size_t t = ((size_t)p[4] << 8) | p[5];
    if (w < 1 || w > MAX_BITMAP_W || t > 400 || 9 + t + (size_t)w * 32 != len) return "ERR";
    const uint8_t* txt = p + 9;
    if (!setBitmap(txt + t, w)) return "ERR";

    char hex[7];
    snprintf(hex, sizeof(hex), "%02x%02x%02x", p[6], p[7], p[8]);
    currentColorHex = hex;
    statusText = "";
    statusText.concat((const char*)txt, t);
    startSong(song);
    return "OK";
  }
  return "ERR";
}

void sendReply(const String& reply) {
  size_t n = reply.length();
  size_t frameLen = 12 + n + 16;
  uint8_t* out = (uint8_t*)malloc(2 + frameLen);
  if (!out) return;
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

  out[0] = frameLen >> 8;
  out[1] = frameLen & 0xFF;
  client.write(out, 2 + frameLen);
  client.flush();
  free(out);
}

void processFrame(const uint8_t* f, size_t n) {
  size_t ptLen = n - 12 - 16;
  uint8_t* pt = (uint8_t*)malloc(ptLen + 1);
  if (!pt) { closeClient(); return; }
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
  int ret = mbedtls_gcm_auth_decrypt(&g, ptLen, f, 12, challenge, sizeof(challenge),
                                     f + n - 16, 16, f + 12, pt);
  mbedtls_gcm_free(&g);
  if (ret != 0) { free(pt); authFailed(); return; }             // wrong key or tampered/replayed

  authFailures = 0;
  String reply = handleRequest(pt, ptLen);
  free(pt);
  sendReply(reply);
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

  if (millis() - clientSince > 10000 || !client.connected()) { closeClient(); return; }

  int avail = client.available();
  if (avail > 0 && rxLen < sizeof(rx)) {
    int r = client.read(rx + rxLen, min((size_t)avail, sizeof(rx) - rxLen));
    if (r > 0) rxLen += r;
  }
  if (rxLen < 2) return;

  size_t n = ((size_t)rx[0] << 8) | rx[1];
  if (n < 12 + 16 + 1 || n > MAX_FRAME) { authFailed(); return; }   // malformed
  if (rxLen >= 2 + n) processFrame(rx + 2, n);
}

// ---------- Network setup (settings stored on the board, edited on a setup page) ----------

struct NetConfig {
  String ssid, pass;
  bool dhcp;
  String ip, gw, mask, dns;
};
NetConfig cfg;
Preferences prefs;
DNSServer dnsServer;
WebServer web(80);
bool portalActive = false;
unsigned long portalStarted = 0;
const unsigned long PORTAL_TIMEOUT = 600000;   // setup page closes after 10 min and the board retries its saved WiFi
String scanOptions;                            // <option> list of nearby networks

void loadConfig() {
  prefs.begin("net", false);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.dhcp = prefs.getBool("dhcp", true);
  cfg.ip   = prefs.getString("ip", "192.168.1.50");
  cfg.gw   = prefs.getString("gw", "192.168.1.1");
  cfg.mask = prefs.getString("mask", "255.255.255.0");
  cfg.dns  = prefs.getString("dns", "8.8.8.8");
}

String esc(const String& s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else               o += c;
  }
  return o;
}

// Starts joining the saved network (fixed IP if configured).
void beginJoin() {
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  if (!cfg.dhcp) {
    IPAddress ip, gw, mask, d;
    if (ip.fromString(cfg.ip) && gw.fromString(cfg.gw) && mask.fromString(cfg.mask) && d.fromString(cfg.dns)) {
      if (!WiFi.config(ip, gw, mask, d)) Serial.println("Static IP config failed");
    } else {
      Serial.println("Saved fixed IP is invalid, using DHCP");
    }
  }
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
}

void scanNetworks() {
  int n = WiFi.scanNetworks();
  scanOptions = "";
  for (int i = 0; i < n && i < 20; i++) scanOptions += "<option value=\"" + esc(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();
}

const char PAGE_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark"><title>LED Matrix setup</title>
<style>
  body{font-family:system-ui,sans-serif;max-width:440px;margin:1.5em auto;padding:0 1em}
  label{display:block;margin:.9em 0 .3em;font-size:.9em;color:#667}
  input[type=text],input[type=password]{width:100%;padding:.65em;font-size:1.05em;box-sizing:border-box}
  button{margin-top:1.2em;width:100%;padding:.75em;font-size:1.05em}
  .row{display:flex;gap:1.2em} .row label{display:inline;margin:0;font-size:1em;color:inherit}
  .msg{padding:.7em;border-radius:6px;background:#fde8e8;color:#8a1f1f} .ok{background:#e6f4ea;color:#14532d}
  small,a{color:#667}
</style></head><body><h2>LED Matrix setup</h2>)rawliteral";

String pageForm(const String& err) {
  String h = FPSTR(PAGE_HEAD);
  if (err.length()) h += "<p class=\"msg\">" + esc(err) + "</p>";
  h += "<form method=\"POST\" action=\"/save\">";
  h += "<label>WiFi name</label><input type=\"text\" name=\"ssid\" list=\"nets\" maxlength=\"32\" required value=\"" + esc(cfg.ssid) + "\">";
  h += "<datalist id=\"nets\">" + scanOptions + "</datalist>";
  h += "<label>WiFi password</label><input type=\"password\" name=\"pass\" maxlength=\"63\" autocomplete=\"off\" placeholder=\"" +
       String(cfg.ssid.length() ? "leave blank to keep the saved password" : "") + "\">";
  h += "<label>IP address</label><div class=\"row\">";
  h += "<span><input type=\"radio\" name=\"mode\" id=\"d\" value=\"dhcp\"" + String(cfg.dhcp ? " checked" : "") + "> <label for=\"d\">Automatic (DHCP)</label></span>";
  h += "<span><input type=\"radio\" name=\"mode\" id=\"s\" value=\"static\"" + String(cfg.dhcp ? "" : " checked") + "> <label for=\"s\">Fixed</label></span></div>";
  h += "<label>Fixed IP</label><input type=\"text\" name=\"ip\" value=\"" + esc(cfg.ip) + "\">";
  h += "<label>Gateway (your router)</label><input type=\"text\" name=\"gw\" value=\"" + esc(cfg.gw) + "\">";
  h += "<label>Subnet mask</label><input type=\"text\" name=\"mask\" value=\"" + esc(cfg.mask) + "\">";
  h += "<label>DNS</label><input type=\"text\" name=\"dns\" value=\"" + esc(cfg.dns) + "\">";
  h += "<button type=\"submit\">Save and restart</button></form>";
  h += "<p><a href=\"/?rescan=1\">Rescan WiFi networks</a></p>";
  h += "<p><small>Fixed IP: pick a free address on your network, outside your router's DHCP range. The three fixed-IP fields are ignored when Automatic is selected.</small></p>";
  h += "</body></html>";
  return h;
}

void handleRoot() {
  if (web.hasArg("rescan") || scanOptions.length() == 0) scanNetworks();
  web.send(200, "text/html", pageForm(""));
}

void handleSave() {
  String ssid = web.arg("ssid"); ssid.trim();
  String pass = web.arg("pass");
  bool dhcp = web.arg("mode") != "static";
  String ip = web.arg("ip"), gw = web.arg("gw"), mask = web.arg("mask"), d = web.arg("dns");
  ip.trim(); gw.trim(); mask.trim(); d.trim();
  if (d.length() == 0) d = gw;

  String err;
  IPAddress a;
  if (ssid.length() == 0 || ssid.length() > 32) err = "Enter the WiFi name (up to 32 characters).";
  else if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63)) err = "WiFi password must be 8-63 characters.";
  else if (!dhcp && !(a.fromString(ip) && a.fromString(gw) && a.fromString(mask) && a.fromString(d)))
    err = "Fixed IP, gateway, subnet mask and DNS must be valid addresses like 192.168.1.50.";
  if (err.length()) {
    cfg.ssid = ssid; cfg.dhcp = dhcp;                           // keep what was typed
    if (ip.length()) cfg.ip = ip;
    if (gw.length()) cfg.gw = gw;
    if (mask.length()) cfg.mask = mask;
    if (d.length()) cfg.dns = d;
    web.send(400, "text/html", pageForm(err));
    return;
  }

  if (pass.length() == 0 && ssid == cfg.ssid) pass = cfg.pass;  // blank = keep the saved password
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putBool("dhcp", dhcp);
  if (!dhcp) {                                                  // keep the old fixed-IP values when switching to DHCP
    prefs.putString("ip", ip);
    prefs.putString("gw", gw);
    prefs.putString("mask", mask);
    prefs.putString("dns", d);
  }

  String h = FPSTR(PAGE_HEAD);
  h += "<p class=\"msg ok\">Saved. The board is restarting and will join <b>" + esc(ssid) + "</b>. You can close this page.</p></body></html>";
  web.send(200, "text/html", h);
  delay(1000);
  ESP.restart();
}

// Hotspot + setup page. alsoJoin = keep trying the saved network in the background (used when joining failed).
void startPortal(bool alsoJoin) {
  portalActive = true;
  portalStarted = millis();
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.disconnect();
  WiFi.softAP(SETUP_AP_NAME, SETUP_AP_PASSWORD);
  delay(200);
  scanNetworks();
  if (alsoJoin) beginJoin();
  dnsServer.start(53, "*", WiFi.softAPIP());                    // phones open the page automatically
  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.onNotFound([]() {
    web.sendHeader("Location", "http://192.168.4.1/");
    web.send(302);
  });
  web.begin();
  Serial.println("Setup mode: join WiFi '" + String(SETUP_AP_NAME) + "' and open http://192.168.4.1");
  setText("Setup: join WiFi " + String(SETUP_AP_NAME) + ", password " + SETUP_AP_PASSWORD + ", open 192.168.4.1",
          matrix.Color(255, 140, 0));
}

void stopPortal() {
  dnsServer.stop();
  web.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalActive = false;
}

// Hold the BOOT button ~3s while the sketch is running to reopen the setup page.
void checkBootButton() {
  static unsigned long since = 0;
  if (digitalRead(BOOT_PIN) == LOW) {
    if (since == 0) since = millis();
    else if (millis() - since > 3000) {
      setText("SETUP", matrix.Color(255, 140, 0));
      prefs.putBool("force", true);
      delay(500);
      ESP.restart();
    }
  } else {
    since = 0;
  }
}

// ---------- Setup / loop ----------

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BOOT_PIN, INPUT_PULLUP);

  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(5);      // keep low; use an external 5V supply for more

  if (!parseKey()) {
    Serial.println("DEVICE_KEY must be exactly 64 hex characters");
    setText("KEY?", matrix.Color(255, 0, 0));   // short so it fits without scrolling
    while (true) delay(1000);
  }

  setText("WiFi...", DEFAULT_COLOR());
  loadConfig();
  bool force = prefs.getBool("force", false);
  if (force) prefs.putBool("force", false);

  if (cfg.ssid.length() == 0 || force) {        // nothing saved yet, or setup requested
    startPortal(false);
    return;
  }

  WiFi.mode(WIFI_STA);
  beginJoin();
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(100);
  if (WiFi.status() != WL_CONNECTED) startPortal(true);   // can't join: open setup page, keep retrying meanwhile
}

void loop() {
  checkBootButton();
  if (portalActive) {
    dnsServer.processNextRequest();
    web.handleClient();
    if (WiFi.status() != WL_CONNECTED && millis() - portalStarted > PORTAL_TIMEOUT) ESP.restart();
  }

  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected && !wasConnected) {
    if (portalActive) stopPortal();             // joined after all: close the setup hotspot
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
