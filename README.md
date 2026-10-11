# pingdot

ESP32 + WS2812B LED matrix, controlled from a website.

- `web/` – Vercel site + API (stores the latest message in Upstash Redis). Env vars: `SITE_PASSWORD`, plus the Upstash `KV_REST_API_URL`/`KV_REST_API_TOKEN` (added by the Vercel Marketplace integration).
- `firmware/` – ESP32 firmware (PlatformIO). Copy `src/secrets.h.example` to `src/secrets.h`, fill in WiFi and your Vercel URL (`/api/latest`). It polls the site every 4s.
