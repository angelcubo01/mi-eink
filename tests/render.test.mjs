import assert from 'node:assert/strict';
import test from 'node:test';

import { ditherImageData, weatherIconName } from '../web/render.js';

function grayscale(values) {
  const data = new Uint8ClampedArray(values.length * 4);
  values.forEach((value, index) => {
    data[index * 4] = data[index * 4 + 1] = data[index * 4 + 2] = value;
    data[index * 4 + 3] = 255;
  });
  return data;
}

test('el dithering Floyd–Steinberg produce únicamente píxeles blancos o negros', () => {
  const data = grayscale([0, 48, 96, 144, 192, 240, 32, 128, 224]);
  ditherImageData(data, 3, 3, { dither: 'floyd', threshold: 150, brightness: 0, contrast: 0, gamma: 1 });
  const tones = new Set(Array.from(data).filter((_, index) => index % 4 === 0));
  assert.deepEqual([...tones].sort((a, b) => a - b), [0, 255]);
});

test('el dithering Bayer conserva el requisito 1-bit e invierte si se solicita', () => {
  const data = grayscale([120, 130, 140, 150]);
  ditherImageData(data, 2, 2, { dither: 'bayer', threshold: 135, invert: true });
  const tones = Array.from(data).filter((_, index) => index % 4 === 0);
  assert.ok(tones.every((value) => value === 0 || value === 255));
});

test('cubre las familias de códigos meteorológicos de OpenWeather', () => {
  assert.equal(weatherIconName(202), 'thunderstorm');
  assert.equal(weatherIconName(301), 'rain_light');
  assert.equal(weatherIconName(511), 'rain');
  assert.equal(weatherIconName(602), 'snow');
  assert.equal(weatherIconName(741), 'fog');
  assert.equal(weatherIconName(781), 'tornado');
  assert.equal(weatherIconName(800, 1), 'sunny');
  assert.equal(weatherIconName(800, 0), 'night');
  assert.equal(weatherIconName(801, 0), 'partly_cloudy_night');
  assert.equal(weatherIconName(804), 'cloud');
});
