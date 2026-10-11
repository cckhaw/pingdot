const { passwordOk, isAuthed, setCookie, clearCookie } = require('./_auth');

// GET: am I logged in?  POST {password}: log in (sets a cookie).  DELETE: log out.
module.exports = async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  if (req.method === 'GET') return res.status(200).json({ ok: isAuthed(req) });
  if (req.method === 'DELETE') {
    clearCookie(res);
    return res.status(200).json({ ok: false });
  }
  if (req.method === 'POST') {
    let body = req.body || {};
    if (typeof body === 'string') {
      try { body = JSON.parse(body); } catch (e) { body = {}; }
    }
    if (!passwordOk(body.password)) return res.status(401).json({ error: 'Wrong password' });
    setCookie(res);
    return res.status(200).json({ ok: true });
  }
  res.setHeader('Allow', 'GET, POST, DELETE');
  return res.status(405).json({ error: 'Method not allowed' });
};
