import assert from 'node:assert/strict';
import test from 'node:test';

import { BMP_SIZE, createDevServer, validateMonoBmp } from '../scripts/dev-server.mjs';

function validBmp() {
  const bmp = Buffer.alloc(BMP_SIZE, 0xff);
  bmp.write('BM');
  bmp.writeUInt32LE(BMP_SIZE, 2);
  bmp.writeUInt32LE(62, 10);
  bmp.writeUInt32LE(40, 14);
  bmp.writeUInt32LE(800, 18);
  bmp.writeUInt32LE(480, 22);
  bmp.writeUInt16LE(1, 26);
  bmp.writeUInt16LE(1, 28);
  bmp.writeUInt32LE(0, 30);
  bmp.writeUInt32LE(48000, 34);
  bmp.set([0, 0, 0, 0, 255, 255, 255, 0], 54);
  return bmp;
}

async function startServer() {
  const { server } = createDevServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();
  return { server, base: `http://127.0.0.1:${port}` };
}

test('el simulador valida el BMP exacto esperado por el firmware', () => {
  const bmp = validBmp();
  assert.equal(validateMonoBmp(bmp), true);
  assert.equal(validateMonoBmp(Buffer.alloc(BMP_SIZE - 1)), false);
  bmp[0] = 0;
  assert.equal(validateMonoBmp(bmp), false);
});

test('la API local simula el ciclo de actualización y rechaza cargas erróneas', async (t) => {
  const { server, base } = await startServer();
  t.after(() => server.close());

  const initial = await (await fetch(`${base}/api/status`)).json();
  assert.equal(initial.state, 'ready');
  assert.equal(initial.simulation, true);

  const wrong = await fetch(`${base}/api/display`, { method: 'POST', headers: { 'Content-Type': 'image/bmp' }, body: Buffer.alloc(10) });
  assert.equal(wrong.status, 400);

  const accepted = await fetch(`${base}/api/display`, { method: 'POST', headers: { 'Content-Type': 'image/bmp' }, body: validBmp() });
  assert.equal(accepted.status, 202);
  const { job } = await accepted.json();
  assert.equal(job, 1);

  const conflict = await fetch(`${base}/api/display`, { method: 'POST', headers: { 'Content-Type': 'image/bmp' }, body: validBmp() });
  assert.equal(conflict.status, 409);

  await new Promise((resolve) => setTimeout(resolve, 720));
  const finished = await (await fetch(`${base}/api/status`)).json();
  assert.equal(finished.state, 'ready');
  assert.equal(finished.job, job);
});
