const { Redis } = require('@upstash/redis');

const url = process.env.KV_REST_API_URL || process.env.UPSTASH_REDIS_REST_URL;
const token = process.env.KV_REST_API_TOKEN || process.env.UPSTASH_REDIS_REST_TOKEN;

const redis = new Redis({ url, token });
const KEY = 'ledmatrix:message';
const MAX_TEXT_LEN = 100;

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

async function getMessage() {
  const m = await redis.get(KEY);
  return m || { id: 0, text: '' };
}

async function setMessage(text) {
  const msg = { id: Date.now(), text };
  await redis.set(KEY, msg);
  return msg;
}

module.exports = { getMessage, setMessage, clean };
