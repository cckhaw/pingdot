# pingdot

ESP32 + WS2812B LED matrix, controlled from a website. The Vercel site sends each message
straight to the ESP32 over an encrypted TCP socket (port-forwarded on your router).

- `web/` – Vercel site + API. No database. Env vars: `SITE_PASSWORD`, `DEVICE_HOST` (public hostname/IP),
  `DEVICE_PORT` (default 8443), `DEVICE_KEY` (64 hex chars, `openssl rand -hex 32`).
- `firmware/ledmatrix/ledmatrix.ino` – ESP32 sketch (Arduino IDE; only the Adafruit GFX/NeoMatrix/NeoPixel
  libraries are needed). Listens on `TCP_PORT`; `DEVICE_KEY` in the sketch must equal the one on Vercel. WiFi name, password and IP are not in the code; see *Network setup*.

## Protocol
`ESP32 -> site`: 16 random bytes (challenge, new per connection).
`site -> ESP32`: `[2-byte length][12-byte IV | AES-256-GCM ciphertext | 16-byte tag]`, key = `DEVICE_KEY`, AAD = challenge.
`ESP32 -> site`: same framing, AAD = challenge + `R`.
Requests: `S` (status) or `B` (a message: song, width, colour, UTF-8 text and a 16-pixel-high RGB565 picture). The browser draws the text itself, so any language and emoji work. The fresh challenge makes recorded requests useless.

## Network setup
WiFi name, password and (optional) fixed IP are entered on the board's own setup page and saved on the board, so you never edit them in the sketch.

- **First boot / no saved WiFi:** the matrix scrolls the setup instructions and the board opens a hotspot `LEDMatrix-Setup` (password `ledmatrix`, change it in the sketch). Join it with a phone or laptop; the setup page opens by itself, or go to `http://192.168.4.1`. Choose your network, enter the password, choose Automatic (DHCP) or a fixed IP, and press *Save and restart*.
- **Can't join the saved WiFi (20 s):** the same setup page opens, while the board keeps retrying in the background and closes the hotspot as soon as it joins. The page also closes itself after 10 minutes and the board retries its saved settings.
- **Change settings later:** hold the board's **BOOT** button for ~3 seconds while it is running (not during reset); it restarts into the setup page.
