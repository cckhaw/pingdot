const { callDevice, clean, cleanColor, cleanSong } = require('./_device');
const { isAuthed } = require('./_auth');

// POST {text, color, song}: log-in required; pushed straight to the ESP32 over an encrypted TCP socket.
module.exports = async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  if (req.method !== 'POST') {
    res.setHeader('Allow', 'POST');
    return res.status(405).json({ error: 'Method not allowed' });
  }
  if (!isAuthed(req)) return res.status(401).json({ error: 'Not logged in' });

  let body = req.body || {};
  if (typeof body === 'string') {
    try { body = JSON.parse(body); } catch (e) { body = {}; }
  }
  const text = clean(body.text);
  if (!text) return res.status(400).json({ error: 'Text is empty' });
  const color = cleanColor(body.color);
  const song = cleanSong(body.song);

  try {
    const r = await callDevice(`M\n${color.slice(1)}\n${song}\n${text}`);
    if (r === 'OK') return res.status(200).json({ ok: true, text, color, song });
    return res.status(502).json({ error: 'Device refused the message' });
  } catch (e) {
    return res.status(502).json({ error: e.message === 'Device is not configured' ? e.message : 'Device unreachable' });
  }
};
