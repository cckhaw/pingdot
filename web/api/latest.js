const { getState, touchDevice } = require('./_store');

// Plain-text endpoint for the ESP32 (no JSON parser needed):
//   "<id>\n<RRGGBB>\n<song>\n<text>"
// The device adds ?hb=1 now and then; that records a heartbeat so the site can show "online".
module.exports = async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  res.setHeader('Content-Type', 'text/plain; charset=utf-8');
  try {
    if (req.query && req.query.hb) await touchDevice();
    const { message: m } = await getState();
    res.status(200).send(`${m.id}\n${String(m.color).replace('#', '')}\n${m.song}\n${m.text}`);
  } catch (e) {
    res.status(500).send('error');
  }
};
