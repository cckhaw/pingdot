# pingdot

ESP32 + WS2812B LED matrix, controlled from a website.

- `web/` – Vercel site + API (latest message, colour and sound stored in Upstash Redis). Env vars: `SITE_PASSWORD`, plus the Upstash `KV_REST_API_URL`/`KV_REST_API_TOKEN` (added by the Vercel Marketplace integration).
- `firmware/ledmatrix/ledmatrix.ino` – ESP32 sketch (Arduino IDE). Fill in WiFi and your Vercel URL (`/api/latest`) at the top. It polls the site every 8s and pings it every ~32s so the site can show "Device online".
