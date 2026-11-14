import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import test from 'node:test';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

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

async function startServer(options = {}) {
  const { server } = createDevServer(options);
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

test('persiste proyectos y recursos sin devolver contraseñas', async (t) => {
  const dataRoot = await mkdtemp(join(tmpdir(), 'mieink-test-'));
  const { server, base } = await startServer({ dataRoot });
  t.after(async () => { server.close(); await rm(dataRoot, { recursive: true, force: true }); });

  const project = await (await fetch(`${base}/api/project`)).json();
  project.name = 'Panel probado';
  project.device.network.apPassword = 'no-debe-guardarse';
  const frame = validBmp();
  const frameId = createHash('sha256').update(frame).digest('hex');
  assert.equal((await fetch(`${base}/api/frames/${frameId}`, { method: 'PUT', headers: { 'Content-Type': 'image/bmp' }, body: frame })).status, 201);
  project.pages[0].frameId = frameId;
  const saved = await (await fetch(`${base}/api/project`, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(project) })).json();
  assert.equal(saved.name, 'Panel probado');
  assert.equal(JSON.stringify(saved).includes('no-debe-guardarse'), false);

  const asset = await (await fetch(`${base}/api/assets/${frameId}`, { method: 'POST', headers: { 'Content-Type': 'image/bmp', 'X-File-Name': 'prueba.bmp' }, body: frame })).json();
  assert.match(asset.id, /^[a-f0-9]{64}$/);
  assert.equal(asset.format, 'mono1');
  assert.equal((await fetch(`${base}${asset.url}`)).status, 200);
  const storage = await (await fetch(`${base}/api/device/storage`)).json();
  assert.equal(storage.reserved, 256 * 1024);
  assert.ok(storage.assetsBytes >= BMP_SIZE);

  const network = await (await fetch(`${base}/api/device/network`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ homeSsid: 'Casa', homePassword: 'secreto2' }) })).json();
  assert.equal(network.secretsAccepted, true);
  assert.equal(JSON.stringify(network).includes('secreto2'), false);

  const status = await (await fetch(`${base}/api/device/network`)).json();
  assert.equal(status.homeSsid, 'Casa');
  assert.equal(status.connected, true);
  const scan = await (await fetch(`${base}/api/device/networks`)).json();
  assert.ok(scan.networks.some((item) => item.ssid === 'Casa'));
});

test('la simulación lee OpenWeather desde secrets.h sin exponer la clave', async (t) => {
  const dataRoot = await mkdtemp(join(tmpdir(), 'mieink-secrets-test-'));
  const secretsPath = join(dataRoot, 'secrets.h');
  await writeFile(secretsPath, '#define MI_EINK_OPENWEATHER_API_KEY "clave-solo-servidor"\n', 'utf8');
  const { server, base } = await startServer({ dataRoot, secretsPath });
  t.after(async () => { server.close(); await rm(dataRoot, { recursive: true, force: true }); });

  const network = await (await fetch(`${base}/api/device/network`)).json();
  assert.equal(network.weatherConfigured, true);
  assert.equal(JSON.stringify(network).includes('clave-solo-servidor'), false);
});

test('normaliza OpenWeather y mantiene una caché meteorológica de una hora', async (t) => {
  let weatherCalls = 0;
  const unix = (value) => Math.floor(Date.parse(value) / 1000);
  const fakeFetch = async (url) => {
    const target = String(url);
    if (target.includes('/geo/1.0/direct')) return { ok: true, json: async () => ([{ name: 'Madrid', local_names: { es: 'Madrid' }, state: 'Comunidad de Madrid', country: 'ES', lat: 40.4, lon: -3.7 }]) };
    if (target.includes('date.nager.at')) return { ok: true, json: async () => ([
      { date: '2026-01-01', localName: 'Año Nuevo', global: true, counties: null },
      { date: '2026-02-28', localName: 'Día de Andalucía', global: false, counties: ['ES-AN'] },
    ]) };
    weatherCalls += 1;
    if (target.includes('/data/2.5/weather')) return { ok: true, json: async () => ({
      dt: unix('2026-11-07T12:00:00Z'), timezone: 0, visibility: 10000,
      main: { temp: 21.4, feels_like: 20.1, temp_min: 11, temp_max: 22, humidity: 66, pressure: 1014 },
      weather: [{ id: 801, description: 'algo de nubes' }], clouds: { all: 45 }, wind: { speed: 3.6667, deg: 210 }, rain: { '1h': 1.2 },
      sys: { sunrise: unix('2026-11-07T07:45:00Z'), sunset: unix('2026-11-07T18:05:00Z') },
    }) };
    return { ok: true, json: async () => ({
      city: { timezone: 0 },
      list: [
        { dt: unix('2026-11-07T15:00:00Z'), main: { temp: 20, temp_min: 18, temp_max: 22, feels_like: 19 }, weather: [{ id: 500, description: 'lluvia ligera' }], clouds: { all: 60 }, wind: { speed: 5.55 }, rain: { '3h': 2.5 }, pop: .7 },
        { dt: unix('2026-11-08T12:00:00Z'), main: { temp: 17, temp_min: 10, temp_max: 18, feels_like: 16 }, weather: [{ id: 800, description: 'cielo claro' }], clouds: { all: 0 }, wind: { speed: 4 }, pop: .1 },
      ],
    }) };
  };
  const { server, base } = await startServer({ fetchImpl: fakeFetch, openWeatherApiKey: 'clave-de-prueba' });
  t.after(() => server.close());
  const places = await (await fetch(`${base}/api/integrations/geocode?q=Madrid`)).json();
  assert.equal(places.results[0].name, 'Madrid');
  const weather = await (await fetch(`${base}/api/integrations/weather?latitude=40.4&longitude=-3.7&units=celsius`)).json();
  assert.equal(weather.temperature, 21.4);
  assert.equal(weather.wind, 13.2);
  assert.equal(weather.daily[0].max, 22);
  assert.equal(weather.apparentTemperature, 20.1);
  assert.equal(weather.humidity, 66);
  assert.equal(weather.daily[0].precipitationProbability, 70);
  assert.match(weather.daily[0].sunset, /^2026-11-07T18:05/);
  assert.equal(weather.provider, 'OpenWeather');
  assert.equal(weather.code, 801);
  const cachedWeather = await (await fetch(`${base}/api/integrations/weather?latitude=40.4&longitude=-3.7&units=celsius`)).json();
  assert.equal(cachedWeather.cached, true);
  assert.equal(weatherCalls, 2);
  const holidays = await (await fetch(`${base}/api/integrations/holidays?year=2026&region=ES-AN`)).json();
  assert.equal(holidays.holidays.length, 2);
  assert.equal(holidays.holidays[1].scope, 'ES-AN');
});

test('expone telemetría simulada sin secretos del dispositivo', async (t) => {
  const { server, base } = await startServer();
  t.after(() => server.close());
  const status = await (await fetch(`${base}/api/device/status`)).json();
  assert.equal(status.source, 'simulation');
  assert.equal(status.power.charging, true);
  assert.equal(typeof status.network.rssi, 'number');
  assert.equal(JSON.stringify(status).includes('Password'), false);
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
  const telemetry = await (await fetch(`${base}/api/device/status`)).json();
  assert.equal(telemetry.display.refreshCount, 1);
  assert.ok(telemetry.display.lastRefreshAt);
});
