const { callDevice } = require('./_device');
const { isAuthed } = require('./_auth');

// Asks the ESP32 what it is showing; no answer = offline. Logged-in users only, and cached
// briefly, so strangers (or many open tabs) can't keep the device busy all day.
let cache = null;   // { at, data }
const CACHE_MS = 10 * 1000;

module.exports = async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  if (!isAuthed(req)) return res.status(401).json({ error: 'Not logged in' });
  if (cache && Date.now() - cache.at < CACHE_MS) return res.status(200).json(cache.data);

  let data;
  try {
    const r = await callDevice('S');
    const nl = r.indexOf('\n');
    data = { online: true, color: '#' + r.slice(0, nl), text: r.slice(nl + 1) };
  } catch (e) {
    data = { online: false, reason: e.message };
  }
  cache = { at: Date.now(), data };
  return res.status(200).json(data);
};
