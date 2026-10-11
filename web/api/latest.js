const { getMessage } = require('./_store');

// Plain-text endpoint for the ESP32 (no JSON parser needed): "<id>\n<text>"
module.exports = async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  res.setHeader('Content-Type', 'text/plain; charset=utf-8');
  try {
    const m = await getMessage();
    res.status(200).send(`${m.id}\n${m.text}`);
  } catch (e) {
    res.status(500).send('error');
  }
};
