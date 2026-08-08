const crypto = require('crypto');
const db = require('./db');

function hashKey(key) {
  return crypto.createHash('sha256').update(key).digest('hex');
}

function generateApiKey() {
  return crypto.randomBytes(24).toString('hex');
}

function requireAdmin(req, res, next) {
  if (req.session && req.session.isAdmin) return next();
  return res.status(401).json({ error: 'not authenticated' });
}

function requireDevice(req, res, next) {
  const id = req.header('X-Device-Id');
  const key = req.header('X-Api-Key');
  if (!id || !key) return res.status(401).json({ error: 'missing device credentials' });

  const row = db.prepare('SELECT * FROM devices WHERE id = ?').get(id);
  if (!row) return res.status(401).json({ error: 'unknown device' });
  if (row.api_key_hash !== hashKey(key)) return res.status(401).json({ error: 'bad api key' });

  req.device = row;
  next();
}

module.exports = { hashKey, generateApiKey, requireAdmin, requireDevice };