const { Redis } = require('@upstash/redis');

const url = process.env.KV_REST_API_URL || process.env.UPSTASH_REDIS_REST_URL;
const token = process.env.KV_REST_API_TOKEN || process.env.UPSTASH_REDIS_REST_TOKEN;

const redis = new Redis({ url, token });
const KEY = 'ledmatrix:message';
const SEEN_KEY = 'ledmatrix:lastSeen';
const MAX_TEXT_LEN = 100;
const SONG_COUNT = 6;            // 0 = none; keep in sync with the firmware and index.html
const DEFAULT_COLOR = '#00c8ff';

// Printable ASCII only (the matrix font has nothing else), same as the firmware.
function clean(t) {
  let out = '';
  for (const ch of String(t || '')) {
    if (out.length >= MAX_TEXT_LEN) break;
    const c = ch.charCodeAt(0);
    out += c >= 32 && c <= 126 ? ch : '?';
  }
  return out.trim();
}

function cleanColor(c) {
  return /^#[0-9a-fA-F]{6}$/.test(String(c)) ? String(c).toLowerCase() : DEFAULT_COLOR;
}

function cleanSong(s) {
  const n = Number.parseInt(s, 10);
  return Number.isInteger(n) && n >= 0 && n < SONG_COUNT ? n : 0;
}

// One Redis command: message + the device's last heartbeat.
async function getState() {
  const [m, seen] = await redis.mget(KEY, SEEN_KEY);
  const message = m || { id: 0, text: '' };
  return {
    message: { color: DEFAULT_COLOR, song: 0, ...message },
    lastSeen: Number(seen) || 0,
  };
}

async function setMessage({ text, color, song }) {
  const msg = { id: Date.now(), text, color, song };
  await redis.set(KEY, msg);
  return msg;
}

async function touchDevice() {
  await redis.set(SEEN_KEY, Date.now(), { ex: 3600 });
}

module.exports = { getState, setMessage, touchDevice, clean, cleanColor, cleanSong };
