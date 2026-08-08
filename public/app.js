const PW = 384, BPR = 48;

function log(msg) {
  const l = document.getElementById('log');
  if (!l) return;
  l.textContent += msg + "\n";
  l.scrollTop = l.scrollHeight;
}

async function requireAuth() {
  const r = await fetch('/api/auth/me');
  const j = await r.json();
  if (!j.authenticated) window.location.href = '/login.html';
}

function wrapText(ctx, text, maxWidth) {
  const out = [];
  text.split(/\r?\n/).forEach(paragraph => {
    if (paragraph.length === 0) { out.push(''); return; }
    const words = paragraph.split(/\s+/);
    let line = '';
    words.forEach(word => {
      const candidate = line ? line + ' ' + word : word;
      if (ctx.measureText(candidate).width <= maxWidth) {
        line = candidate;
      } else {
        if (line) out.push(line);
        line = word;
      }
    });
    if (line) out.push(line);
  });
  return out;
}

function renderTextToCanvas(text, fontSize) {
  const measure = document.createElement('canvas').getContext('2d');
  measure.font = fontSize + 'px monospace';
  const lines = wrapText(measure, text, PW - 8);
  const lineHeight = Math.ceil(fontSize * 1.25);
  const height = Math.max(lines.length * lineHeight + 12, 10);

  const canvas = document.createElement('canvas');
  canvas.width = PW;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = 'white';
  ctx.fillRect(0, 0, PW, height);
  ctx.fillStyle = 'black';
  ctx.font = fontSize + 'px monospace';
  ctx.textBaseline = 'top';
  lines.forEach((line, i) => ctx.fillText(line, 4, 6 + i * lineHeight));
  return canvas;
}

function canvasToBitmap(canvas) {
  const h = canvas.height;
  const ctx = canvas.getContext('2d');
  const img = ctx.getImageData(0, 0, PW, h);
  const bitmap = new Uint8Array(BPR * h);
  for (let y = 0; y < h; y++) {
    for (let xByte = 0; xByte < BPR; xByte++) {
      let value = 0;
      for (let bit = 0; bit < 8; bit++) {
        const x = xByte * 8 + bit;
        const idx = (y * PW + x) * 4;
        const lum = 0.299 * img.data[idx] + 0.587 * img.data[idx + 1] + 0.114 * img.data[idx + 2];
        if (lum < 128) value |= (1 << (7 - bit));
      }
      bitmap[y * BPR + xByte] = value;
    }
  }
  return bitmap;
}

function bitmapToBase64(bitmap) {
  let binary = '';
  for (let i = 0; i < bitmap.length; i++) binary += String.fromCharCode(bitmap[i]);
  return btoa(binary);
}

async function submitJob(deviceId, canvas) {
  const bitmap = canvasToBitmap(canvas);
  const r = await fetch('/api/print', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      deviceId,
      width: PW,
      height: canvas.height,
      bitmapBase64: bitmapToBase64(bitmap)
    })
  });
  const j = await r.json();
  if (!j.ok) throw new Error(j.error || 'submit failed');
  return j.jobId;
}