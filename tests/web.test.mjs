import assert from 'node:assert/strict';
import test from 'node:test';

import {
  BMP_SIZE,
  DISPLAY_HEIGHT,
  DISPLAY_WIDTH,
  MARGINS,
  calculateLayout,
  canvasToMonoBmp,
  wrapText,
} from '../web/app.js';

class MeasuringContext {
  constructor() {
    this.font = '700 12px Arial';
  }

  measureText(value) {
    const match = this.font.match(/(\d+)px/);
    const size = match ? Number(match[1]) : 12;
    return { width: Array.from(value).length * size * 0.58 };
  }
}

function createCanvas(blackPixels = []) {
  const pixels = new Uint8ClampedArray(DISPLAY_WIDTH * DISPLAY_HEIGHT * 4);
  for (let offset = 0; offset < pixels.length; offset += 4) {
    pixels[offset] = 255;
    pixels[offset + 1] = 255;
    pixels[offset + 2] = 255;
    pixels[offset + 3] = 255;
  }

  for (const [x, y] of blackPixels) {
    const offset = (y * DISPLAY_WIDTH + x) * 4;
    pixels[offset] = 0;
    pixels[offset + 1] = 0;
    pixels[offset + 2] = 0;
  }

  return {
    width: DISPLAY_WIDTH,
    height: DISPLAY_HEIGHT,
    getContext() {
      return {
        getImageData() {
          return { data: pixels };
        },
      };
    },
  };
}

function u16(data, offset) {
  return new DataView(data.buffer, data.byteOffset, data.byteLength).getUint16(offset, true);
}

function u32(data, offset) {
  return new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(offset, true);
}

test('el texto corto usa una tipografía grande y cabe dentro del área segura', () => {
  const ctx = new MeasuringContext();
  const layout = calculateLayout(ctx, 'Hola');
  assert.ok(layout.fontSize > 100);
  assert.ok(layout.width <= DISPLAY_WIDTH - MARGINS.left - MARGINS.right);
  assert.ok(layout.height <= DISPLAY_HEIGHT - MARGINS.top - MARGINS.bottom);
});

test('respeta saltos manuales y admite caracteres españoles', () => {
  const ctx = new MeasuringContext();
  ctx.font = '700 80px Arial';
  const lines = wrapText(ctx, 'Información mañana\n10 € — España', 900);
  assert.deepEqual(lines, ['Información mañana', '10 € — España']);
});

test('divide una palabra larga sin salirse del ancho', () => {
  const ctx = new MeasuringContext();
  ctx.font = '700 60px Arial';
  const lines = wrapText(ctx, 'supercalifragilisticoespialidosoininterrumpido', 240);
  assert.ok(lines.length > 1);
  assert.ok(lines.every((line) => ctx.measureText(line).width <= 240));
});

test('un párrafo largo reduce el tamaño hasta caber con los márgenes', () => {
  const ctx = new MeasuringContext();
  const text = 'Este es un mensaje largo para comprobar que todas las líneas permanecen dentro de la zona segura de la pantalla. '.repeat(4);
  const layout = calculateLayout(ctx, text);
  assert.equal(layout.fits, true);
  assert.ok(layout.width <= 736);
  assert.ok(layout.height <= 384);
});

test('un texto vacío no produce líneas ni tamaño de fuente', () => {
  const layout = calculateLayout(new MeasuringContext(), '   \n  ');
  assert.equal(layout.fontSize, 0);
  assert.deepEqual(layout.lines, []);
});

test('genera un BMP monocromo estándar de 48062 bytes', () => {
  const bmp = canvasToMonoBmp(createCanvas());
  assert.equal(bmp.byteLength, BMP_SIZE);
  assert.equal(String.fromCharCode(bmp[0], bmp[1]), 'BM');
  assert.equal(u32(bmp, 2), BMP_SIZE);
  assert.equal(u32(bmp, 10), 62);
  assert.equal(u32(bmp, 18), 800);
  assert.equal(u32(bmp, 22), 480);
  assert.equal(u16(bmp, 26), 1);
  assert.equal(u16(bmp, 28), 1);
  assert.equal(u32(bmp, 34), 48000);
  assert.deepEqual(Array.from(bmp.slice(54, 62)), [0, 0, 0, 0, 255, 255, 255, 0]);
});

test('escribe las filas de abajo arriba y los bits de izquierda a derecha', () => {
  const bmp = canvasToMonoBmp(createCanvas([[0, 0]]));
  const rowBytes = DISPLAY_WIDTH / 8;
  const bottomRowFirstByte = bmp[62];
  const topRowFirstByte = bmp[62 + (DISPLAY_HEIGHT - 1) * rowBytes];
  assert.equal(bottomRowFirstByte, 0xff);
  assert.equal(topRowFirstByte, 0x7f);
});
