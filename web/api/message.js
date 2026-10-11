const crypto = require('crypto');
const { getMessage, setMessage, clean } = require('./_store');

function passwordOk(given) {
  const expected = process.env.SITE_PASSWORD || '';
  if (!expected) return false;
  const a = crypto.createHash('sha256').update(String(given || '')).digest();
  const b = crypto.createHash('sha256').update(expected).digest();
  return crypto.timingSafeEqual(a, b);
}

module.exports = async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  try {
    if (req.method === 'GET') {
      return res.status(200).json(await getMessage());
    }
    if (req.method === 'POST') {
      const body = typeof req.body === 'string' ? JSON.parse(req.body || '{}') : req.body || {};
      if (!passwordOk(body.password)) return res.status(401).json({ error: 'Wrong password' });
      const text = clean(body.text);
      if (!text) return res.status(400).json({ error: 'Text is empty' });
      return res.status(200).json(await setMessage(text));
    }
    res.setHeader('Allow', 'GET, POST');
    return res.status(405).json({ error: 'Method not allowed' });
  } catch (e) {
    return res.status(500).json({ error: 'Server error' });
  }
};
