const crypto = require('crypto');

const COOKIE = 'pd_auth';
const MAX_AGE = 60 * 60 * 24 * 365;

function sha(s) {
  return crypto.createHash('sha256').update(String(s)).digest();
}

function safeEqual(a, b) {
  return crypto.timingSafeEqual(sha(a), sha(b));
}

// Session token derived from the password: changing SITE_PASSWORD logs everyone out.
function sessionToken() {
  const pw = process.env.SITE_PASSWORD || '';
  if (!pw) return null;
  return crypto.createHmac('sha256', pw).update('pingdot-session-v1').digest('hex');
}

function passwordOk(given) {
  const pw = process.env.SITE_PASSWORD || '';
  return !!pw && safeEqual(given || '', pw);
}

function isAuthed(req) {
  const t = sessionToken();
  if (!t) return false;
  const m = (req.headers.cookie || '').match(/(?:^|;\s*)pd_auth=([a-f0-9]+)/);
  return !!m && safeEqual(m[1], t);
}

function setCookie(res) {
  res.setHeader('Set-Cookie',
    `${COOKIE}=${sessionToken()}; Path=/; Max-Age=${MAX_AGE}; HttpOnly; Secure; SameSite=Strict`);
}

function clearCookie(res) {
  res.setHeader('Set-Cookie', `${COOKIE}=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Strict`);
}

module.exports = { passwordOk, isAuthed, setCookie, clearCookie };
