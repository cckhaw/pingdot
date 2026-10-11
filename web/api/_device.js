const https = require('https');

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

// HTTPS request to the ESP32. The device's self-signed certificate is the only trusted
// CA (DEVICE_CERT_PEM), so this both encrypts and pins the connection. Hostname checking
// is skipped because the cert is for your home address, which can change (DDNS).
function callDevice(method, path, body = '') {
  return new Promise((resolve, reject) => {
    const ca = (process.env.DEVICE_CERT_PEM || '').replace(/\\n/g, '\n');
    if (!process.env.DEVICE_HOST || !process.env.DEVICE_SECRET || !ca) {
      return reject(new Error('Device is not configured'));
    }
    const req = https.request({
      host: process.env.DEVICE_HOST,
      port: Number(process.env.DEVICE_PORT || 8443),
      path, method, ca,
      checkServerIdentity: () => undefined,
      agent: false,
      timeout: 7000,
      headers: {
        'X-Auth': process.env.DEVICE_SECRET,
        'Content-Type': 'text/plain',
        'Content-Length': Buffer.byteLength(body),
      },
    }, res => {
      let data = '';
      res.setEncoding('utf8');
      res.on('data', d => { if (data.length < 2000) data += d; });
      res.on('end', () => resolve({ status: res.statusCode, body: data }));
    });
    req.on('timeout', () => req.destroy(new Error('Timed out')));
    req.on('error', reject);
    req.end(body);
  });
}

module.exports = { callDevice, clean, cleanColor, cleanSong };
