export const DISPLAY_WIDTH = 800;
export const DISPLAY_HEIGHT = 480;
export const MARGINS = Object.freeze({ top: 48, right: 32, bottom: 48, left: 32 });
export const BMP_SIZE = 48062;

const FONT_FAMILY = 'Arial, Helvetica, sans-serif';
const FONT_WEIGHT = 700;
const MIN_FONT_SIZE = 12;
const MAX_FONT_SIZE = 360;

function graphemes(value) {
  if (typeof Intl !== 'undefined' && Intl.Segmenter) {
    const segmenter = new Intl.Segmenter('es', { granularity: 'grapheme' });
    return Array.from(segmenter.segment(value), (entry) => entry.segment);
  }
  return Array.from(value);
}

function splitLongToken(ctx, token, maxWidth) {
  const parts = [];
  let current = '';

  for (const glyph of graphemes(token)) {
    const candidate = current + glyph;
    if (current && ctx.measureText(candidate).width > maxWidth) {
      parts.push(current);
      current = glyph;
    } else {
      current = candidate;
    }
  }

  if (current) parts.push(current);
  return parts.length ? parts : [''];
}

export function wrapText(ctx, text, maxWidth) {
  const normalized = text.replace(/\r\n?/g, '\n').trim();
  if (!normalized) return [];

  const result = [];
  const paragraphs = normalized.split('\n');

  for (const paragraph of paragraphs) {
    const cleaned = paragraph.trim().replace(/\s+/g, ' ');
    if (!cleaned) {
      result.push('');
      continue;
    }

    const words = cleaned.split(' ');
    let line = '';

    for (const word of words) {
      const pieces = ctx.measureText(word).width > maxWidth ? splitLongToken(ctx, word, maxWidth) : [word];

      for (const piece of pieces) {
        const candidate = line ? `${line} ${piece}` : piece;
        if (line && ctx.measureText(candidate).width > maxWidth) {
          result.push(line);
          line = piece;
        } else {
          line = candidate;
        }
      }
    }

    result.push(line);
  }

  return result;
}

function layoutAtSize(ctx, text, fontSize) {
  ctx.font = `${FONT_WEIGHT} ${fontSize}px ${FONT_FAMILY}`;
  const maxWidth = DISPLAY_WIDTH - MARGINS.left - MARGINS.right;
  const maxHeight = DISPLAY_HEIGHT - MARGINS.top - MARGINS.bottom;
  const lines = wrapText(ctx, text, maxWidth);
  const lineHeight = Math.ceil(fontSize * 1.08);
  const height = lines.length * lineHeight;
  const width = lines.reduce((largest, line) => Math.max(largest, ctx.measureText(line).width), 0);

  return {
    fontSize,
    lineHeight,
    lines,
    width,
    height,
    fits: lines.length > 0 && width <= maxWidth && height <= maxHeight,
  };
}

export function calculateLayout(ctx, text) {
  const normalized = text.replace(/\r\n?/g, '\n').trim();
  if (!normalized) {
    return { fontSize: 0, lineHeight: 0, lines: [], width: 0, height: 0, fits: true };
  }

  let low = MIN_FONT_SIZE;
  let high = MAX_FONT_SIZE;
  let best = layoutAtSize(ctx, normalized, MIN_FONT_SIZE);

  while (low <= high) {
    const middle = Math.floor((low + high) / 2);
    const candidate = layoutAtSize(ctx, normalized, middle);
    if (candidate.fits) {
      best = candidate;
      low = middle + 1;
    } else {
      high = middle - 1;
    }
  }

  return best;
}

export function renderText(canvas, text) {
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  ctx.save();
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);

  const layout = calculateLayout(ctx, text);
  if (layout.lines.length) {
    ctx.fillStyle = '#000000';
    ctx.font = `${FONT_WEIGHT} ${layout.fontSize}px ${FONT_FAMILY}`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';

    const firstY = DISPLAY_HEIGHT / 2 - layout.height / 2 + layout.lineHeight / 2;
    layout.lines.forEach((line, index) => {
      ctx.fillText(line, DISPLAY_WIDTH / 2, firstY + index * layout.lineHeight);
    });
  }

  ctx.restore();
  return layout;
}

function writeU16(view, offset, value) {
  view.setUint16(offset, value, true);
}

function writeU32(view, offset, value) {
  view.setUint32(offset, value, true);
}

export function canvasToMonoBmp(canvas, threshold = 160) {
  if (canvas.width !== DISPLAY_WIDTH || canvas.height !== DISPLAY_HEIGHT) {
    throw new Error('El lienzo debe medir exactamente 800 × 480 píxeles.');
  }

  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  const pixels = ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT).data;
  const rowBytes = DISPLAY_WIDTH / 8;
  const pixelBytes = rowBytes * DISPLAY_HEIGHT;
  const buffer = new ArrayBuffer(62 + pixelBytes);
  const view = new DataView(buffer);
  const output = new Uint8Array(buffer);

  output[0] = 0x42;
  output[1] = 0x4d;
  writeU32(view, 2, buffer.byteLength);
  writeU32(view, 10, 62);
  writeU32(view, 14, 40);
  writeU32(view, 18, DISPLAY_WIDTH);
  writeU32(view, 22, DISPLAY_HEIGHT);
  writeU16(view, 26, 1);
  writeU16(view, 28, 1);
  writeU32(view, 30, 0);
  writeU32(view, 34, pixelBytes);
  writeU32(view, 38, 2835);
  writeU32(view, 42, 2835);
  writeU32(view, 46, 2);
  writeU32(view, 50, 2);

  // Paleta BMP: índice 0 negro, índice 1 blanco (BGRA).
  output.set([0, 0, 0, 0, 255, 255, 255, 0], 54);
  output.fill(0xff, 62);

  for (let bmpRow = 0; bmpRow < DISPLAY_HEIGHT; bmpRow += 1) {
    const sourceY = DISPLAY_HEIGHT - 1 - bmpRow;
    const targetOffset = 62 + bmpRow * rowBytes;

    for (let x = 0; x < DISPLAY_WIDTH; x += 1) {
      const sourceOffset = (sourceY * DISPLAY_WIDTH + x) * 4;
      const luminance =
        pixels[sourceOffset] * 0.2126 +
        pixels[sourceOffset + 1] * 0.7152 +
        pixels[sourceOffset + 2] * 0.0722;

      if (pixels[sourceOffset + 3] > 0 && luminance < threshold) {
        output[targetOffset + (x >> 3)] &= ~(0x80 >> (x & 7));
      }
    }
  }

  return new Uint8Array(buffer);
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function readJson(response) {
  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(data.message || `Error HTTP ${response.status}`);
  }
  return data;
}

async function waitForJob(job) {
  const started = Date.now();
  while (Date.now() - started < 45000) {
    await delay(500);
    const status = await readJson(await fetch('/api/status', { cache: 'no-store' }));
    if (status.job === job && status.state === 'ready') return status;
    if (status.job === job && status.state === 'error') {
      throw new Error(status.message || 'La pantalla no pudo actualizarse.');
    }
  }
  throw new Error('La actualización está tardando demasiado. Comprueba la pantalla.');
}

function initializeApp() {
  const textarea = document.querySelector('#message');
  const canvas = document.querySelector('#preview');
  const sendButton = document.querySelector('#send');
  const counter = document.querySelector('#counter');
  const fontSize = document.querySelector('#font-size');
  const status = document.querySelector('#status');
  const statusText = status.querySelector('.status-text');
  const connectionMode = document.querySelector('#connection-mode');
  const connectionTarget = document.querySelector('#connection-target');
  const isLocalSimulation = ['localhost', '127.0.0.1', '::1'].includes(window.location.hostname);
  let busy = false;

  if (isLocalSimulation) {
    document.body.dataset.mode = 'simulation';
    connectionMode.textContent = 'Simulación local · sin pantalla física';
    connectionTarget.textContent = 'localhost:5173';
  }

  const setStatus = (message, state = '') => {
    statusText.textContent = message;
    if (state) status.dataset.state = state;
    else delete status.dataset.state;
  };

  const refreshPreview = () => {
    const value = textarea.value.slice(0, 500);
    if (value !== textarea.value) textarea.value = value;
    const layout = renderText(canvas, value);
    counter.textContent = `${value.length} / 500`;
    fontSize.textContent = layout.fontSize ? `${layout.fontSize} px` : '— px';
    sendButton.disabled = busy || !value.trim();
    try {
      localStorage.setItem('eink-texto', value);
    } catch (_) {
      // El almacenamiento local es opcional.
    }
  };

  textarea.addEventListener('input', refreshPreview);

  sendButton.addEventListener('click', async () => {
    if (busy || !textarea.value.trim()) return;
    busy = true;
    sendButton.disabled = true;
    setStatus('Preparando imagen monocroma…', 'busy');

    try {
      renderText(canvas, textarea.value);
      const bmp = canvasToMonoBmp(canvas);
      if (bmp.byteLength !== BMP_SIZE) throw new Error('La imagen generada tiene un tamaño incorrecto.');

      setStatus(isLocalSimulation ? 'Enviando a la simulación local…' : 'Enviando al controlador…', 'busy');
      const accepted = await readJson(
        await fetch('/api/display', {
          method: 'POST',
          headers: { 'Content-Type': 'image/bmp' },
          body: bmp,
        }),
      );

      setStatus(isLocalSimulation ? 'Simulando refresco e-ink…' : 'Actualizando la tinta electrónica…', 'busy');
      await waitForJob(accepted.job);
      setStatus(isLocalSimulation ? 'Simulación completada' : 'Pantalla actualizada', 'success');
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'No se pudo actualizar la pantalla.', 'error');
    } finally {
      busy = false;
      sendButton.disabled = !textarea.value.trim();
    }
  });

  try {
    textarea.value = localStorage.getItem('eink-texto') || '';
  } catch (_) {
    textarea.value = '';
  }
  refreshPreview();
}

if (typeof document !== 'undefined') {
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initializeApp, { once: true });
  } else {
    initializeApp();
  }
}
