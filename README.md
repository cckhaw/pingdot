# pingdot

ESP32 + WS2812B LED matrix, controlled from a website. The Vercel site sends each message
over HTTPS straight to the ESP32 (port-forwarded on your router).

- `web/` – Vercel site + API. No database. Env vars: `SITE_PASSWORD`, `DEVICE_HOST` (public hostname/IP),
  `DEVICE_PORT` (default 8443), `DEVICE_SECRET`, `DEVICE_CERT_PEM`.
- `firmware/ledmatrix/ledmatrix.ino` – ESP32 sketch (Arduino IDE, board package ESP32 2.0.17, library
  "ESP32 HTTPS Server" by Frank Hessel). Serves `POST /message` and `GET /status` over TLS; requests need
  the `X-Auth: <DEVICE_SECRET>` header.
- `tools/gen-cert.sh <hostname-or-ip>` – creates the ESP32's self-signed certificate (`cert.h` for the
  sketch, PEM for Vercel). Vercel trusts only that certificate.
