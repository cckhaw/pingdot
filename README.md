# pingdot

ESP32 + WS2812B LED matrix, controlled from a website. The Vercel site sends each message
straight to the ESP32 over an encrypted TCP socket (port-forwarded on your router).

- `web/` – Vercel site + API. No database. Env vars: `SITE_PASSWORD`, `DEVICE_HOST` (public hostname/IP),
  `DEVICE_PORT` (default 8443), `DEVICE_KEY` (64 hex chars, `openssl rand -hex 32`).
- `firmware/ledmatrix/ledmatrix.ino` – ESP32 sketch (Arduino IDE; only the Adafruit GFX/NeoMatrix/NeoPixel
  libraries are needed). Listens on `TCP_PORT`; `DEVICE_KEY` in the sketch must equal the one on Vercel.

## Protocol
`ESP32 -> site`: 16 random bytes (challenge, new per connection).
`site -> ESP32`: `[2-byte length][12-byte IV | AES-256-GCM ciphertext | 16-byte tag]`, key = `DEVICE_KEY`, AAD = challenge.
`ESP32 -> site`: same framing, AAD = challenge + `R`.
Requests: `M\n<RRGGBB>\n<song>\n<text>` (show) or `S` (status). The fresh challenge makes recorded requests useless.
