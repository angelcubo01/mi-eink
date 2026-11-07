#!/usr/bin/env node
/** Servidor de desarrollo local para la interfaz MiEInk, sin hardware. */

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

export const DISPLAY_WIDTH = 800;
export const DISPLAY_HEIGHT = 480;
export const BMP_SIZE = 48062;
const ROOT = resolve(fileURLToPath(new URL('..', import.meta.url)));
const WEB_ROOT = resolve(ROOT, 'web');
const MIME_TYPES = Object.freeze({
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.ico': 'image/x-icon',
});

function json(response, statusCode, payload) {
  response.writeHead(statusCode, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
  response.end(JSON.stringify(payload));
}

function readU16(buffer, offset) {
  return buffer.readUInt16LE(offset);
}

function readU32(buffer, offset) {
  return buffer.readUInt32LE(offset);
}

export function validateMonoBmp(buffer) {
  if (!Buffer.isBuffer(buffer) || buffer.length !== BMP_SIZE) return false;
  if (buffer[0] !== 0x42 || buffer[1] !== 0x4d) return false;
  if (readU32(buffer, 2) !== BMP_SIZE || readU32(buffer, 10) !== 62 || readU32(buffer, 14) !== 40) return false;
  if (readU32(buffer, 18) !== DISPLAY_WIDTH || readU32(buffer, 22) !== DISPLAY_HEIGHT) return false;
  if (readU16(buffer, 26) !== 1 || readU16(buffer, 28) !== 1) return false;
  if (readU32(buffer, 30) !== 0 || readU32(buffer, 34) !== 48000) return false;
  return buffer[54] === 0 && buffer[55] === 0 && buffer[56] === 0 &&
    buffer[58] === 255 && buffer[59] === 255 && buffer[60] === 255;
}

function collectBody(request, limit = BMP_SIZE + 1) {
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

function createSimulation() {
  const state = { state: 'ready', message: 'Simulación local preparada', job: 0, width: DISPLAY_WIDTH, height: DISPLAY_HEIGHT, ip: 'localhost', simulation: true };
  let lastImage = null;
  return {
    status: () => ({ ...state }),
    accept(image) {
      state.job += 1;
      state.state = 'queued';
      state.message = 'Simulación en cola';
      const job = state.job;
      setTimeout(() => {
        if (state.job !== job) return;
        state.state = 'updating';
        state.message = 'Simulando refresco e-ink';
      }, 80);
      setTimeout(() => {
        if (state.job !== job) return;
        lastImage = image;
        state.state = 'ready';
        state.message = 'Simulación completada';
      }, 650);
      return job;
    },
    lastImage: () => lastImage,
  };
}

async function serveStatic(pathname, response) {
  const requested = pathname === '/' ? 'index.html' : decodeURIComponent(pathname).replace(/^\/+/, '');
  const filePath = resolve(WEB_ROOT, requested);
  if (!filePath.startsWith(`${WEB_ROOT}${sep}`) && filePath !== WEB_ROOT) {
    json(response, 403, { message: 'Ruta no permitida.' });
    return;
  }
  try {
    const content = await readFile(filePath);
    response.writeHead(200, { 'Content-Type': MIME_TYPES[extname(filePath)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
    response.end(content);
  } catch {
    json(response, 404, { message: 'Archivo no encontrado.' });
  }
}

export function createDevServer() {
  const simulation = createSimulation();
  const server = createServer(async (request, response) => {
    const url = new URL(request.url || '/', 'http://localhost');

    if (request.method === 'GET' && url.pathname === '/api/status') {
      json(response, 200, simulation.status());
      return;
    }
    if (request.method === 'POST' && url.pathname === '/api/display') {
      if (!request.headers['content-type']?.startsWith('image/bmp')) {
        json(response, 400, { message: 'Content-Type debe ser image/bmp.' });
        return;
      }
      if (simulation.status().state !== 'ready') {
        json(response, 409, { message: 'Ya hay una actualización en curso.' });
        return;
      }
      try {
        const body = await collectBody(request);
        if (!validateMonoBmp(body)) {
          json(response, 400, { message: 'BMP no válido: se requiere 800x480, monocromo y sin compresión.' });
          return;
        }
        const job = simulation.accept(body);
        json(response, 202, { message: 'Actualización simulada aceptada', job });
      } catch (error) {
        json(response, 400, { message: error instanceof Error ? error.message : 'No se pudo leer la imagen.' });
      }
      return;
    }
    if (url.pathname.startsWith('/api/')) {
      json(response, 404, { message: 'Ruta API no encontrada.' });
      return;
    }
    await serveStatic(url.pathname, response);
  });
  return { server, simulation };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const port = Number.parseInt(process.env.PORT || '5173', 10);
  const { server } = createDevServer();
  server.listen(port, '127.0.0.1', () => {
    console.log(`MiEInk en modo simulación: http://localhost:${port}`);
    console.log('Pulsa Ctrl+C para detener el servidor.');
  });
}
