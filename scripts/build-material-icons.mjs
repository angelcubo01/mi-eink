#!/usr/bin/env node
/** Genera un módulo local y pequeño a partir de Material Symbols en Iconify. */

import { writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(fileURLToPath(new URL('..', import.meta.url)));
const OUTPUT = resolve(ROOT, 'web', 'material-icons.js');

// Alias estables para no acoplar el resto de la aplicación a los nombres del set.
const ICONS = Object.freeze({
  undo: 'undo-rounded',
  redo: 'redo-rounded',
  import: 'upload-file-rounded',
  export: 'download-rounded',
  agenda: 'calendar-month-rounded',
  settings: 'settings-rounded',
  add: 'add-rounded',
  duplicate: 'content-copy-rounded',
  delete: 'delete-rounded',
  text: 'text-fields-rounded',
  clock: 'schedule-rounded',
  weather: 'partly-cloudy-day-rounded',
  calendar: 'calendar-month-rounded',
  events: 'event-note-rounded',
  device: 'developer-board-rounded',
  image: 'image-rounded',
  qr: 'qr-code-2-rounded',
  design: 'dashboard-customize-rounded',
  preview: 'monochrome-photos-rounded',
  send: 'send-rounded',
  move_up: 'arrow-upward-rounded',
  move_down: 'arrow-downward-rounded',
  lock: 'lock-rounded',
  unlock: 'lock-open-rounded',
  close: 'close-rounded',
  refresh: 'refresh-rounded',
  location: 'location-on-rounded',
  humidity: 'humidity-percentage-rounded',
  wind: 'air-rounded',
  cloud: 'cloud-rounded',
  sunny: 'sunny-rounded',
  rain: 'rainy-rounded',
  rain_light: 'rainy-light-rounded',
  temperature: 'thermostat-rounded',
  feels: 'device-thermostat-rounded',
  battery: 'battery-full-rounded',
  wifi: 'wifi-rounded',
  chip: 'memory-rounded',
  screen: 'monitor-rounded',
  alert: 'warning-rounded',
  snow: 'weather-snowy-rounded',
  thunderstorm: 'thunderstorm-rounded',
  fog: 'foggy-rounded',
  night: 'dark-mode-rounded',
  partly_cloudy_night: 'partly-cloudy-night-rounded',
  holiday: 'calendar-add-on-rounded',
  date: 'event-rounded',
  edit_calendar: 'edit-calendar-rounded',
  precipitation: 'water-drop-rounded',
  sunrise: 'wb-twilight-rounded',
  visibility: 'visibility-rounded',
  tornado: 'tornado-rounded',
  mist: 'mist-rounded',
});

async function loadIcon(name) {
  const response = await fetch(`https://api.iconify.design/material-symbols/${name}.svg`);
  if (!response.ok) throw new Error(`No se pudo descargar material-symbols:${name} (${response.status}).`);
  const svg = await response.text();
  const viewBox = svg.match(/viewBox="([^"]+)"/)?.[1] || '0 0 24 24';
  const paths = [...svg.matchAll(/<path[^>]*\sd="([^"]+)"[^>]*>/g)].map((match) => match[1]);
  if (!paths.length) throw new Error(`material-symbols:${name} no contiene trazados SVG.`);
  return { viewBox, paths };
}

const entries = await Promise.all(Object.entries(ICONS).map(async ([alias, name]) => [alias, await loadIcon(name)]));
const payload = Object.fromEntries(entries);
const source = `// Generado por scripts/build-material-icons.mjs desde Iconify · Material Symbols.\n`
  + `export const MATERIAL_ICONS = Object.freeze(${JSON.stringify(payload, null, 2)});\n\n`
  + `function escapeAttribute(value) { return String(value ?? '').replace(/[&<>\"']/g, (character) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '\"': '&quot;', "'": '&#39;' })[character]); }\n\n`
  + `export function iconMarkup(name, className = '') {\n`
  + `  const icon = MATERIAL_ICONS[name] || MATERIAL_ICONS.alert;\n`
  + `  return \`<svg class="material-icon \${escapeAttribute(className)}" viewBox="\${icon.viewBox}" aria-hidden="true" focusable="false">\${icon.paths.map((path) => \`<path d="\${path}"></path>\`).join('')}</svg>\`;\n`
  + `}\n\n`
  + `export function hydrateMaterialIcons(root = document) {\n`
  + `  root.querySelectorAll('[data-icon]').forEach((element) => { element.innerHTML = iconMarkup(element.dataset.icon); });\n`
  + `}\n\n`
  + `export function drawMaterialIcon(ctx, name, x, y, size) {\n`
  + `  const icon = MATERIAL_ICONS[name] || MATERIAL_ICONS.alert;\n`
  + `  if (typeof Path2D !== 'function') return;\n`
  + `  const [, , width = 24, height = 24] = icon.viewBox.split(/\\s+/).map(Number);\n`
  + `  ctx.save();\n`
  + `  ctx.translate(x - size / 2, y - size / 2);\n`
  + `  ctx.scale(size / width, size / height);\n`
  + `  icon.paths.forEach((path) => ctx.fill(new Path2D(path)));\n`
  + `  ctx.restore();\n`
  + `}\n`;

await writeFile(OUTPUT, source, 'utf8');
console.log(`Material Symbols generados: ${entries.length} iconos en ${OUTPUT}`);
