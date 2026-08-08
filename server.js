require('dotenv').config();
const express = require('express');
const session = require('express-session');
const path = require('path');
const db = require('./db');
const { hashKey, generateApiKey, requireAdmin, requireDevice } = require('./auth');

const app = express();
app.use(express.json({ limit: '2mb' })); // bitmap base64 payloads can be a few hundred KB
app.use(session({
  secret: process.env.SESSION_SECRET || 'change-me',
  resave: false,
  saveUninitialized: false,
  cookie: { httpOnly: true, maxAge: 1000 * 60 * 60 * 24 * 30 }
}));

// ---- Admin auth (this is YOUR login to the dashboard) ----
app.post('/api/auth/login', (req, res) => {
  const { password } = req.body;
  if (!process.env.ADMIN_PASSWORD) {
    return res.status(500).json({ error: 'server has no ADMIN_PASSWORD configured' });
  }
  if (password !== process.env.ADMIN_PASSWORD) {
    return res.status(401).json({ error: 'wrong password' });
  }
  req.session.isAdmin = true;
  res.json({ ok: true });
});

app.post('/api/auth/logout', (req, res) => {
  req.session.destroy(() => res.json({ ok: true }));
});

app.get('/api/auth/me', (req, res) => {
  res.json({ authenticated: !!(req.session && req.session.isAdmin) });
});

// ---- Devices (admin only) ----
app.get('/api/devices', requireAdmin, (req, res) => {
  const rows = db.prepare('SELECT id, name, created_at FROM devices').all();
  res.json(rows);
});

app.post('/api/devices', requireAdmin, (req, res) => {
  const { id, name } = req.body;
  if (!id) return res.status(400).json({ error: 'id required' });
  const existing = db.prepare('SELECT id FROM devices WHERE id = ?').get(id);
  if (existing) return res.status(409).json({ error: 'device id already exists' });

  const apiKey = generateApiKey();
  db.prepare('INSERT INTO devices (id, name, api_key_hash, created_at) VALUES (?, ?, ?, ?)')
    .run(id, name || id, hashKey(apiKey), Date.now());

  // apiKey is only ever returned here, right after creation - store it now
  res.json({ id, name: name || id, apiKey });
});

app.delete('/api/devices/:id', requireAdmin, (req, res) => {
  db.prepare('DELETE FROM devices WHERE id = ?').run(req.params.id);
  db.prepare('DELETE FROM jobs WHERE device_id = ?').run(req.params.id);
  res.json({ ok: true });
});

// ---- Jobs: admin submits + views, device polls + acks ----
app.post('/api/print', requireAdmin, (req, res) => {
  const { deviceId, width, height, bitmapBase64 } = req.body;
  if (!deviceId || !height || !bitmapBase64) {
    return res.status(400).json({ error: 'deviceId, height, bitmapBase64 required' });
  }
  const device = db.prepare('SELECT id FROM devices WHERE id = ?').get(deviceId);
  if (!device) return res.status(404).json({ error: 'unknown device' });

  const bitmap = Buffer.from(bitmapBase64, 'base64');
  const w = width || 384;
  const expected = Math.ceil(w / 8) * height;
  if (bitmap.length < expected) {
    return res.status(400).json({ error: `bitmap too short: got ${bitmap.length}, expected ${expected}` });
  }

  const now = Date.now();
  const info = db.prepare(
    `INSERT INTO jobs (device_id, status, width, height, bitmap, created_at, updated_at)
     VALUES (?, 'queued', ?, ?, ?, ?, ?)`
  ).run(deviceId, w, height, bitmap, now, now);

  res.json({ ok: true, jobId: info.lastInsertRowid });
});

app.get('/api/jobs', requireAdmin, (req, res) => {
  const deviceId = req.query.deviceId;
  const cols = 'id, device_id, status, width, height, error, created_at, updated_at';
  const rows = deviceId
    ? db.prepare(`SELECT ${cols} FROM jobs WHERE device_id = ? ORDER BY id DESC LIMIT 50`).all(deviceId)
    : db.prepare(`SELECT ${cols} FROM jobs ORDER BY id DESC LIMIT 50`).all();
  res.json(rows);
});

app.delete('/api/jobs/:id', requireAdmin, (req, res) => {
  db.prepare(`DELETE FROM jobs WHERE id = ? AND status = 'queued'`).run(req.params.id);
  res.json({ ok: true });
});

// ---- Device polling API (auth via X-Device-Id / X-Api-Key headers) ----
app.get('/api/device/next-job', requireDevice, (req, res) => {
  const job = db.prepare(
    `SELECT id, width, height, bitmap FROM jobs WHERE device_id = ? AND status = 'queued' ORDER BY id ASC LIMIT 1`
  ).get(req.device.id);

  if (!job) return res.status(204).end();

  db.prepare(`UPDATE jobs SET status = 'printing', updated_at = ? WHERE id = ?`)
    .run(Date.now(), job.id);

  res.json({
    id: job.id,
    width: job.width,
    height: job.height,
    bitmap: job.bitmap.toString('base64')
  });
});

app.post('/api/device/jobs/:id/ack', requireDevice, (req, res) => {
  const { status, error } = req.body; // 'done' | 'failed'
  const job = db.prepare('SELECT * FROM jobs WHERE id = ? AND device_id = ?').get(req.params.id, req.device.id);
  if (!job) return res.status(404).json({ error: 'job not found' });

  db.prepare('UPDATE jobs SET status = ?, error = ?, updated_at = ? WHERE id = ?')
    .run(status === 'done' ? 'done' : 'failed', error || null, Date.now(), job.id);

  res.json({ ok: true });
});

// ---- Static website ----
app.use(express.static(path.join(__dirname, 'public')));

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => console.log(`CTP500 cloud server listening on :${PORT}`));