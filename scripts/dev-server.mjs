#!/usr/bin/env node
/** Servidor de desarrollo y API simulada de MiEInk Studio. */

import { randomUUID } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { mkdir, readFile, readdir, unlink, writeFile } from 'node:fs/promises';
import { extname, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createDefaultProject, sanitizeProject } from '../web/model.js';

export const DISPLAY_WIDTH = 800;
export const DISPLAY_HEIGHT = 480;
export const BMP_SIZE = 48062;

const ROOT = resolve(fileURLToPath(new URL('..', import.meta.url)));
const WEB_ROOT = resolve(ROOT, 'web');
const DEFAULT_DATA_ROOT = resolve(ROOT, '.data');
const QR_LIBRARY = resolve(ROOT, 'node_modules', 'qrcode-generator', 'qrcode.js');
const DEFAULT_SECRETS_FILE = resolve(ROOT, 'include', 'secrets.h');
const MIME_TYPES = Object.freeze({
  '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8', '.js': 'application/javascript; charset=utf-8',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.webp': 'image/webp', '.ico': 'image/x-icon',
});

function json(response, statusCode, payload) {
  response.writeHead(statusCode, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
  response.end(JSON.stringify(payload));
}

function secretDefine(path, name) {
  if (!path) return '';
  try {
    const source = readFileSync(path, 'utf8');
    const match = source.match(new RegExp(`^\\s*#define\\s+${name}\\s+"([^"]*)"`, 'm'));
    return String(match?.[1] || '').trim();
  } catch {
    return '';
  }
}

function openWeatherKey(options) {
  if (Object.hasOwn(options, 'openWeatherApiKey')) return String(options.openWeatherApiKey || '').trim();
  const environmentKey = String(process.env.OPENWEATHER_API_KEY || '').trim();
  if (environmentKey) return environmentKey;
  const secretsPath = options.secretsPath === false ? '' : resolve(options.secretsPath || DEFAULT_SECRETS_FILE);
  return secretDefine(secretsPath, 'MI_EINK_OPENWEATHER_API_KEY');
}

function readU16(buffer, offset) { return buffer.readUInt16LE(offset); }
function readU32(buffer, offset) { return buffer.readUInt32LE(offset); }

export function validateMonoBmp(buffer) {
  if (!Buffer.isBuffer(buffer) || buffer.length !== BMP_SIZE) return false;
  if (buffer[0] !== 0x42 || buffer[1] !== 0x4d) return false;
  if (readU32(buffer, 2) !== BMP_SIZE || readU32(buffer, 10) !== 62 || readU32(buffer, 14) !== 40) return false;
  if (readU32(buffer, 18) !== DISPLAY_WIDTH || readU32(buffer, 22) !== DISPLAY_HEIGHT) return false;
  if (readU16(buffer, 26) !== 1 || readU16(buffer, 28) !== 1) return false;
  if (readU32(buffer, 30) !== 0 || readU32(buffer, 34) !== 48000) return false;
  return buffer[54] === 0 && buffer[55] === 0 && buffer[56] === 0 && buffer[58] === 255 && buffer[59] === 255 && buffer[60] === 255;
}

function collectBody(request, limit) {
  return new Promise((resolveBody, rejectBody) => {
    const chunks = [];
    let size = 0;
    request.on('data', (chunk) => {
      size += chunk.length;
      if (size > limit) {
        rejectBody(new Error('El cuerpo de la petición supera el tamaño permitido.'));
        request.destroy();
        return;
      }
      chunks.push(chunk);
    });
    request.on('end', () => resolveBody(Buffer.concat(chunks)));
    request.on('error', rejectBody);
  });
}

async function readJsonBody(request, limit = 1024 * 1024) {
  const body = await collectBody(request, limit);
  try { return JSON.parse(body.toString('utf8')); }
  catch { throw new Error('El cuerpo JSON no es válido.'); }
}

function createSimulation() {
  const state = { state: 'ready', message: 'Simulación local preparada', job: 0, width: DISPLAY_WIDTH, height: DISPLAY_HEIGHT, ip: 'localhost', simulation: true };
  const startedAt = Date.now();
  let lastRefreshAt = null;
  let refreshCount = 0;
  let lastImage = null;
  return {
    status: () => ({ ...state }),
    accept(image) {
      state.job += 1;
      state.state = 'queued';
      state.message = 'Simulación en cola';
      const job = state.job;
      setTimeout(() => { if (state.job === job) { state.state = 'updating'; state.message = 'Simulando refresco e-ink'; } }, 80);
      setTimeout(() => { if (state.job === job) { lastImage = image; lastRefreshAt = new Date().toISOString(); refreshCount += 1; state.state = 'ready'; state.message = 'Simulación completada'; } }, 650);
      return job;
    },
    lastImage: () => lastImage,
    telemetry(project) {
      const uptimeSeconds = Math.max(0, Math.floor((Date.now() - startedAt) / 1000));
      const minutes = Math.floor(uptimeSeconds / 60);
      const battery = Math.max(45, 82 - Math.floor(uptimeSeconds / 3600));
      return {
        source: 'simulation', updatedAt: new Date().toISOString(), connected: true, error: null,
        power: { percentage: battery, charging: true, voltage: 4.08 },
        network: { state: 'ap', ssid: project.device.network.apSsid || 'MiEInk', ip: 'localhost', rssi: -48 - (minutes % 7) },
        display: { state: state.state, lastRefreshAt, refreshCount, refreshMinutes: project.device.display.refreshMinutes || 0 },
        system: { uptimeSeconds, uptimeLabel: `${Math.floor(uptimeSeconds / 3600)} h ${Math.floor((uptimeSeconds % 3600) / 60)} min`, firmware: 'MiEInk Studio sim', temperatureC: 27 + (minutes % 3) },
      };
    },
  };
}

function projectStore(dataRoot) {
  const projectFile = resolve(dataRoot, 'project.json');
  const assetsRoot = resolve(dataRoot, 'assets');
  return {
    async read() {
      try { return sanitizeProject(JSON.parse(await readFile(projectFile, 'utf8'))); }
      catch { return createDefaultProject(); }
    },
    async write(project) {
      await mkdir(dataRoot, { recursive: true });
      const clean = sanitizeProject(project);
      await writeFile(projectFile, `${JSON.stringify(clean, null, 2)}\n`, 'utf8');
      return clean;
    },
    async saveAsset(buffer, mime, name) {
      const extension = mime === 'image/png' ? '.png' : mime === 'image/webp' ? '.webp' : '.jpg';
      const assetId = randomUUID();
      await mkdir(assetsRoot, { recursive: true });
      await writeFile(resolve(assetsRoot, `${assetId}${extension}`), buffer);
      return { id: assetId, name: String(name || 'imagen').slice(0, 120), mime, size: buffer.length, url: `/api/assets/${assetId}` };
    },
    async findAsset(assetId) {
      if (!/^[a-f0-9-]{36}$/i.test(assetId)) return null;
      try {
        const file = (await readdir(assetsRoot)).find((entry) => entry.startsWith(`${assetId}.`));
        return file ? resolve(assetsRoot, file) : null;
      } catch { return null; }
    },
    async deleteAsset(assetId) {
      const path = await this.findAsset(assetId);
      if (!path) return false;
      await unlink(path);
      return true;
    },
  };
}

async function serveFile(path, response, cache = 'no-store') {
  try {
    const content = await readFile(path);
    response.writeHead(200, { 'Content-Type': MIME_TYPES[extname(path).toLowerCase()] || 'application/octet-stream', 'Cache-Control': cache });
    response.end(content);
  } catch { json(response, 404, { message: 'Archivo no encontrado.' }); }
}

async function serveStatic(pathname, response) {
  if (pathname === '/vendor/qrcode.js') return serveFile(QR_LIBRARY, response, 'public, max-age=3600');
  const requested = pathname === '/' ? 'index.html' : decodeURIComponent(pathname).replace(/^\/+/, '');
  const filePath = resolve(WEB_ROOT, requested);
  if (!filePath.startsWith(`${WEB_ROOT}${sep}`) && filePath !== WEB_ROOT) return json(response, 403, { message: 'Ruta no permitida.' });
  return serveFile(filePath, response);
}

function assetIdFrom(pathname) {
  const match = pathname.match(/^\/api\/assets\/([a-f0-9-]{36})$/i);
  return match?.[1] || null;
}

function safeDevice(project) {
  return { simulation: true, connected: false, display: project.device.display, network: project.device.network, buttons: project.device.buttons };
}

async function externalJson(fetchImpl, url) {
  const response = await fetchImpl(url, { headers: { 'User-Agent': 'MiEInk-Studio/1.0' } });
  if (!response.ok) throw new Error(`El servicio externo respondió ${response.status}.`);
  return response.json();
}

function openWeatherSpeed(value, units) {
  const speed = Number(value);
  if (!Number.isFinite(speed)) return null;
  return units === 'fahrenheit' ? speed : speed * 3.6;
}

function precipitationOf(entry = {}) {
  return Number(entry.rain?.['1h'] ?? entry.rain?.['3h'] ?? 0) + Number(entry.snow?.['1h'] ?? entry.snow?.['3h'] ?? 0);
}

function isoAtOffset(unixSeconds, offsetSeconds) {
  if (!Number.isFinite(Number(unixSeconds))) return null;
  return new Date((Number(unixSeconds) + Number(offsetSeconds || 0)) * 1000).toISOString().replace('Z', '');
}

function openWeatherPayload(current, forecast, units = 'celsius', requestedDays = 3) {
  const offset = Number(forecast.city?.timezone ?? current.timezone ?? 0);
  const dateKey = (unixSeconds) => isoAtOffset(unixSeconds, offset)?.slice(0, 10);
  const currentDate = dateKey(current.dt);
  const groups = new Map();
  const ensureDay = (date) => {
    if (!groups.has(date)) groups.set(date, {
      date,
      label: new Intl.DateTimeFormat('es-ES', { weekday: 'short', timeZone: 'UTC' }).format(new Date(`${date}T12:00:00Z`)),
      code: null, condition: '', min: Infinity, max: -Infinity, apparentMin: Infinity, apparentMax: -Infinity,
      precipitation: 0, precipitationProbability: 0, wind: 0, sunrise: null, sunset: null, representativeDistance: Infinity,
    });
    return groups.get(date);
  };
  const addEntry = (entry, isCurrent = false) => {
    const date = dateKey(entry.dt);
    if (!date) return;
    const day = ensureDay(date);
    const main = entry.main || {};
    const temperature = Number(main.temp);
    const min = Number(main.temp_min ?? temperature);
    const max = Number(main.temp_max ?? temperature);
    const apparent = Number(main.feels_like ?? temperature);
    if (Number.isFinite(min)) day.min = Math.min(day.min, min);
    if (Number.isFinite(max)) day.max = Math.max(day.max, max);
    if (Number.isFinite(apparent)) { day.apparentMin = Math.min(day.apparentMin, apparent); day.apparentMax = Math.max(day.apparentMax, apparent); }
    day.precipitation += precipitationOf(entry);
    day.precipitationProbability = Math.max(day.precipitationProbability, Math.round(Number(entry.pop || 0) * 100));
    day.wind = Math.max(day.wind, openWeatherSpeed(entry.wind?.speed, units) || 0);
    const localHour = Number(isoAtOffset(entry.dt, offset)?.slice(11, 13));
    const distance = isCurrent ? -1 : Math.abs(localHour - 12);
    if (distance < day.representativeDistance) {
      day.code = entry.weather?.[0]?.id ?? null;
      day.condition = entry.weather?.[0]?.description || '';
      day.representativeDistance = distance;
    }
  };
  addEntry(current, true);
  for (const entry of forecast.list || []) addEntry(entry);
  if (currentDate && groups.has(currentDate)) {
    const today = groups.get(currentDate);
    today.sunrise = isoAtOffset(current.sys?.sunrise, offset);
    today.sunset = isoAtOffset(current.sys?.sunset, offset);
  }
  const daily = [...groups.values()].sort((a, b) => a.date.localeCompare(b.date)).slice(0, requestedDays).map((day) => ({
    ...day,
    min: Number.isFinite(day.min) ? day.min : null,
    max: Number.isFinite(day.max) ? day.max : null,
    apparentMin: Number.isFinite(day.apparentMin) ? day.apparentMin : null,
    apparentMax: Number.isFinite(day.apparentMax) ? day.apparentMax : null,
    precipitation: Number(day.precipitation.toFixed(2)),
    wind: Number(day.wind.toFixed(1)),
    representativeDistance: undefined,
  }));
  const isDay = Number(current.dt) >= Number(current.sys?.sunrise) && Number(current.dt) < Number(current.sys?.sunset);
  return {
    provider: 'OpenWeather',
    fetchedAt: new Date().toISOString(),
    units: { temperature: units === 'fahrenheit' ? '°F' : '°C', wind: units === 'fahrenheit' ? 'mph' : 'km/h', precipitation: units === 'fahrenheit' ? 'in' : 'mm' },
    temperature: current.main?.temp,
    apparentTemperature: current.main?.feels_like,
    humidity: current.main?.humidity,
    pressure: current.main?.pressure,
    precipitation: Number(precipitationOf(current).toFixed(2)),
    precipitationProbability: daily[0]?.precipitationProbability ?? 0,
    cloudCover: current.clouds?.all,
    visibility: current.visibility == null ? null : Number(current.visibility) / 1000,
    code: current.weather?.[0]?.id,
    condition: current.weather?.[0]?.description || '',
    wind: openWeatherSpeed(current.wind?.speed, units) == null ? null : Number(openWeatherSpeed(current.wind?.speed, units).toFixed(1)),
    windDirection: current.wind?.deg,
    windGust: openWeatherSpeed(current.wind?.gust, units) == null ? null : Number(openWeatherSpeed(current.wind?.gust, units).toFixed(1)),
    isDay: isDay ? 1 : 0,
    daily,
  };
}

function holidayPayload(data, region) {
  return data.filter((holiday) => {
    if (holiday.global || holiday.nationalHoliday) return true;
    const subdivisions = Array.isArray(holiday.counties) ? holiday.counties : holiday.subdivisionCodes;
    return region && Array.isArray(subdivisions) && subdivisions.includes(region);
  }).map((holiday) => ({
    id: `es-${holiday.date}-${String(holiday.localName || holiday.name).replace(/[^a-z0-9]+/gi, '-').toLowerCase()}`,
    date: holiday.date,
    title: holiday.localName || holiday.name || 'Festivo',
    scope: (holiday.global || holiday.nationalHoliday) ? 'Nacional' : region,
  }));
}

export function createDevServer(options = {}) {
  const simulation = createSimulation();
  const store = projectStore(options.dataRoot || DEFAULT_DATA_ROOT);
  const fetchImpl = options.fetchImpl || globalThis.fetch;
  const openWeatherApiKey = openWeatherKey(options);
  const weatherCache = new Map();
  const server = createServer(async (request, response) => {
    const url = new URL(request.url || '/', 'http://localhost');
    try {
      if (request.method === 'GET' && url.pathname === '/api/status') return json(response, 200, simulation.status());
      if (request.method === 'POST' && url.pathname === '/api/display') {
        if (!request.headers['content-type']?.startsWith('image/bmp')) return json(response, 400, { message: 'Content-Type debe ser image/bmp.' });
        if (simulation.status().state !== 'ready') return json(response, 409, { message: 'Ya hay una actualización en curso.' });
        const body = await collectBody(request, BMP_SIZE + 1);
        if (!validateMonoBmp(body)) return json(response, 400, { message: 'BMP no válido: se requiere 800x480, monocromo y sin compresión.' });
        const job = simulation.accept(body);
        return json(response, 202, { message: 'Actualización simulada aceptada', job });
      }
      if (url.pathname === '/api/project' && request.method === 'GET') return json(response, 200, await store.read());
      if (url.pathname === '/api/project' && request.method === 'PUT') return json(response, 200, await store.write(await readJsonBody(request)));
      if (url.pathname === '/api/assets' && request.method === 'POST') {
        const mime = String(request.headers['content-type'] || '').split(';')[0];
        if (!['image/png', 'image/jpeg', 'image/webp'].includes(mime)) return json(response, 415, { message: 'Solo se admiten PNG, JPEG y WebP.' });
        const body = await collectBody(request, 4 * 1024 * 1024 + 1);
        const name = decodeURIComponent(String(request.headers['x-file-name'] || 'imagen'));
        const asset = await store.saveAsset(body, mime, name);
        return json(response, 201, asset);
      }
      const assetId = assetIdFrom(url.pathname);
      if (assetId && request.method === 'GET') {
        const path = await store.findAsset(assetId);
        return path ? serveFile(path, response, 'public, max-age=31536000, immutable') : json(response, 404, { message: 'Recurso no encontrado.' });
      }
      if (assetId && request.method === 'DELETE') return json(response, await store.deleteAsset(assetId) ? 200 : 404, { deleted: true });
      if (url.pathname === '/api/device/status' && request.method === 'GET') return json(response, 200, simulation.telemetry(await store.read()));
      if (url.pathname === '/api/device/network' && request.method === 'GET') {
        const project = await store.read();
        return json(response, 200, { mode: 'ap_sta', apSsid: project.device.network.apSsid || 'MiEInk', apActive: true, apIp: 'localhost', homeSsid: project.device.network.homeSsid || '', configured: Boolean(project.device.network.homeSsid), connected: Boolean(project.device.network.homeSsid), state: project.device.network.homeSsid ? 'connected' : 'not_configured', error: null, disconnectReason: null, ip: 'localhost', rssi: -48, weatherConfigured: Boolean(openWeatherApiKey), simulation: true });
      }
      if (url.pathname === '/api/device/networks' && request.method === 'GET') {
        const project = await store.read();
        const selected = project.device.network.homeSsid;
        return json(response, 200, { connectedSsid: selected || '', networks: [
          ...(selected ? [{ ssid: selected, rssi: -48, security: 'wpa2', open: false, configured: true }] : []),
          { ssid: 'WiFi de ejemplo', rssi: -61, security: 'wpa2', open: false, configured: false },
        ] });
      }
      if (url.pathname === '/api/device' && request.method === 'GET') return json(response, 200, safeDevice(await store.read()));
      if (url.pathname === '/api/device' && request.method === 'PUT') {
        const body = await readJsonBody(request, 128 * 1024);
        const project = await store.read();
        if (body.display && typeof body.display === 'object') project.device.display = { ...project.device.display, ...body.display, width: DISPLAY_WIDTH, height: DISPLAY_HEIGHT };
        if (body.buttons && typeof body.buttons === 'object') project.device.buttons = body.buttons;
        return json(response, 200, safeDevice(await store.write(project)));
      }
      if (url.pathname === '/api/device/network' && request.method === 'POST') {
        const body = await readJsonBody(request, 16 * 1024);
        const project = await store.read();
        const homeSsid = String(body.homeSsid || '').trim().slice(0, 32);
        const password = String(body.homePassword || '');
        if (!homeSsid) return json(response, 400, { message: 'Selecciona o escribe una red Wi-Fi válida.' });
        if (password && (password.length < 8 || password.length > 63)) return json(response, 400, { message: 'La contraseña Wi-Fi debe tener entre 8 y 63 caracteres.' });
        project.device.network = { mode: 'ap_sta', apSsid: project.device.network.apSsid || 'MiEInk', homeSsid };
        await store.write(project);
        return json(response, 200, { simulation: true, network: project.device.network, secretsAccepted: Boolean(body.homePassword) });
      }
      if (url.pathname === '/api/integrations/geocode' && request.method === 'GET') {
        if (!openWeatherApiKey) { const error = new Error('Falta MI_EINK_OPENWEATHER_API_KEY en include/secrets.h.'); error.statusCode = 503; throw error; }
        const query = String(url.searchParams.get('q') || '').trim();
        if (query.length < 2) return json(response, 400, { message: 'Escribe al menos dos caracteres.' });
        const target = new URL('https://api.openweathermap.org/geo/1.0/direct');
        target.searchParams.set('q', query);
        target.searchParams.set('limit', '5');
        target.searchParams.set('appid', openWeatherApiKey);
        const data = await externalJson(fetchImpl, target);
        const results = (Array.isArray(data) ? data : []).map((place) => ({
          name: place.local_names?.es || place.name,
          state: place.state || '',
          country: place.country || '',
          latitude: place.lat,
          longitude: place.lon,
        }));
        return json(response, 200, { results });
      }
      if (url.pathname === '/api/integrations/weather' && request.method === 'GET') {
        if (!openWeatherApiKey) { const error = new Error('Falta MI_EINK_OPENWEATHER_API_KEY en include/secrets.h.'); error.statusCode = 503; throw error; }
        const latitude = Number(url.searchParams.get('latitude'));
        const longitude = Number(url.searchParams.get('longitude'));
        if (!Number.isFinite(latitude) || !Number.isFinite(longitude)) return json(response, 400, { message: 'Coordenadas no válidas.' });
        const fahrenheit = url.searchParams.get('units') === 'fahrenheit';
        const days = Math.max(1, Math.min(5, Number(url.searchParams.get('days') || 3)));
        const units = fahrenheit ? 'fahrenheit' : 'celsius';
        const cacheKey = [latitude.toFixed(4), longitude.toFixed(4), units, days].join(':');
        const cached = weatherCache.get(cacheKey);
        if (cached && Date.now() - cached.storedAt < 60 * 60 * 1000) return json(response, 200, { ...cached.payload, cached: true });
        const query = { lat: String(latitude), lon: String(longitude), appid: openWeatherApiKey, units: fahrenheit ? 'imperial' : 'metric', lang: 'es' };
        const currentTarget = new URL('https://api.openweathermap.org/data/2.5/weather');
        const forecastTarget = new URL('https://api.openweathermap.org/data/2.5/forecast');
        for (const [key, value] of Object.entries(query)) { currentTarget.searchParams.set(key, value); forecastTarget.searchParams.set(key, value); }
        const [current, forecast] = await Promise.all([externalJson(fetchImpl, currentTarget), externalJson(fetchImpl, forecastTarget)]);
        const payload = openWeatherPayload(current, forecast, units, days);
        weatherCache.set(cacheKey, { storedAt: Date.now(), payload });
        return json(response, 200, { ...payload, cached: false });
      }
      if (url.pathname === '/api/integrations/holidays' && request.method === 'GET') {
        const year = Number(url.searchParams.get('year'));
        const region = String(url.searchParams.get('region') || '');
        if (!Number.isInteger(year) || year < 2000 || year > 2100) return json(response, 400, { message: 'Año no válido.' });
        if (region && !/^ES-[A-Z]{2}$/.test(region)) return json(response, 400, { message: 'Comunidad autónoma no válida.' });
        const data = await externalJson(fetchImpl, `https://date.nager.at/api/v3/PublicHolidays/${year}/ES`);
        return json(response, 200, { year, region, holidays: holidayPayload(data, region) });
      }
      if (url.pathname.startsWith('/api/')) return json(response, 404, { message: 'Ruta API no encontrada.' });
      return serveStatic(url.pathname, response);
    } catch (error) {
      return json(response, Number(error?.statusCode) || 400, { message: error instanceof Error ? error.message : 'No se pudo procesar la petición.' });
    }
  });
  return { server, simulation, store };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const port = Number.parseInt(process.env.PORT || '51731', 10);
  const { server } = createDevServer();
  server.listen(port, '127.0.0.1', () => {
    console.log(`MiEInk Studio en modo simulación: http://localhost:${port}`);
    console.log('Pulsa Ctrl+C para detener el servidor.');
  });
}
