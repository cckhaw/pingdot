const { getState, setMessage, clean, cleanColor, cleanSong } = require('./_store');
const { isAuthed } = require('./_auth');

const ONLINE_WINDOW_MS = 45 * 1000;   // device pings every ~32s

module.exports = async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  try {
    if (req.method === 'GET') {
      const { message, lastSeen } = await getState();
      const age = lastSeen ? Date.now() - lastSeen : null;
      return res.status(200).json({
        ...message,
        device: { online: age !== null && age < ONLINE_WINDOW_MS, lastSeenAgoSec: age === null ? null : Math.round(age / 1000) },
      });
    }
    if (req.method === 'POST') {
      if (!isAuthed(req)) return res.status(401).json({ error: 'Not logged in' });
      let body = req.body || {};
      if (typeof body === 'string') {
        try { body = JSON.parse(body); } catch (e) { body = {}; }
      }
      const text = clean(body.text);
      if (!text) return res.status(400).json({ error: 'Text is empty' });
      return res.status(200).json(await setMessage({
        text, color: cleanColor(body.color), song: cleanSong(body.song),
      }));
    }
    res.setHeader('Allow', 'GET, POST');
    return res.status(405).json({ error: 'Method not allowed' });
  } catch (e) {
    return res.status(500).json({ error: 'Server error' });
  }
};
