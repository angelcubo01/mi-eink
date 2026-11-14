import assert from 'node:assert/strict';
import test from 'node:test';

import {
  addPage,
  applyButtonAction,
  canPlace,
  createDefaultProject,
  createWidget,
  deletePage,
  dynamicScheduleForPage,
  sanitizeProject,
} from '../web/model.js';

test('crea widgets en huecos libres e impide colisiones', () => {
  const project = createDefaultProject();
  const page = project.pages[0];
  const first = createWidget('text', page, { x: 0, y: 0, w: 8, h: 3 });
  page.widgets.push(first);
  assert.equal(canPlace(page, { x: 0, y: 0, w: 2, h: 2 }), false);
  assert.equal(canPlace(page, { x: 8, y: 0, w: 2, h: 2 }), true);
  const second = createWidget('qr', page);
  assert.ok(second);
  assert.notEqual(second.x, first.x);
  assert.equal(second.w, 2);
  assert.equal(second.h, 2);
});

test('crea, duplica y elimina páginas sin permitir quedarse sin ninguna', () => {
  const project = createDefaultProject();
  project.pages[0].widgets.push(createWidget('clock', project.pages[0]));
  const copy = addPage(project, project.pages[0].id);
  assert.equal(project.pages.length, 2);
  assert.equal(copy.widgets.length, 1);
  assert.notEqual(copy.widgets[0].id, project.pages[0].widgets[0].id);
  assert.equal(deletePage(project, copy.id), true);
  assert.equal(deletePage(project, project.pages[0].id), false);
});

test('las acciones de botones recorren páginas, invierten y blanquean', () => {
  const project = createDefaultProject();
  addPage(project);
  const second = project.activePageId;
  project.activePageId = project.pages[0].id;
  let runtime = applyButtonAction(project, 'key2', {});
  assert.equal(project.activePageId, second);
  project.device.buttons.key1.action = 'toggle_invert';
  runtime = applyButtonAction(project, 'key1', runtime);
  assert.equal(runtime.inverted, true);
  project.device.buttons.key3.action = 'blank';
  runtime = applyButtonAction(project, 'key3', runtime);
  assert.equal(runtime.blank, true);
  project.device.buttons.key2.action = 'next_page';
  runtime = applyButtonAction(project, 'key2', runtime);
  assert.equal(runtime.blank, false);
});

test('sanitiza el proyecto y elimina contraseñas de red', () => {
  const clean = sanitizeProject({
    ...createDefaultProject(),
    device: { ...createDefaultProject().device, network: { mode: 'ap_sta', apSsid: 'MiEInk', homeSsid: 'Casa', apPassword: 'secreto', homePassword: 'otro' } },
  });
  assert.deepEqual(clean.device.network, { mode: 'ap_sta', apSsid: 'MiEInk', homeSsid: 'Casa' });
  assert.equal(JSON.stringify(clean).includes('secreto'), false);
});

test('descarta widgets solapados al importar un proyecto', () => {
  const project = createDefaultProject();
  const page = project.pages[0];
  page.widgets = [
    createWidget('text', page, { x: 0, y: 0, w: 8, h: 3 }),
    createWidget('clock', page, { x: 0, y: 0, w: 6, h: 3 }),
  ];
  assert.equal(sanitizeProject(project).pages[0].widgets.length, 1);
});

test('migra proyectos 16x10 al grid 20x12 y sustituye el intervalo por planificación inteligente', () => {
  const legacy = createDefaultProject();
  legacy.schemaVersion = 1;
  legacy.pages[0].widgets = [{ id: 'legacy', type: 'text', x: 8, y: 5, w: 4, h: 2, config: { text: 'Hola' } }];
  legacy.device.display = { width: 800, height: 480, refresh: 'automatic', refreshMinutes: 17 };
  const migrated = sanitizeProject(legacy);
  assert.equal(migrated.schemaVersion, 7);
  assert.deepEqual(migrated.pages[0].widgets[0] && { x: migrated.pages[0].widgets[0].x, y: migrated.pages[0].widgets[0].y, w: migrated.pages[0].widgets[0].w, h: migrated.pages[0].widgets[0].h }, { x: 10, y: 6, w: 5, h: 2 });
  assert.equal(migrated.device.display.refreshMode, 'full');
  assert.equal('refreshMinutes' in migrated.device.display, false);
  assert.equal('refresh' in migrated.device.display, false);
});

test('migra los eventos heredados a una agenda global y separa Calendario de Eventos', () => {
  const legacy = createDefaultProject();
  legacy.schemaVersion = 2;
  legacy.pages[0].widgets = [
    { id: 'agenda-antigua', type: 'calendar', x: 0, y: 0, w: 4, h: 3, config: { view: 'agenda', events: [{ date: '2026-12-24', title: 'Nochebuena' }] } },
    { id: 'mes-antiguo', type: 'calendar', x: 5, y: 0, w: 8, h: 5, config: { view: 'month', events: [{ date: '2026-12-24', title: 'Nochebuena' }] } },
  ];
  const migrated = sanitizeProject(legacy);
  assert.equal(migrated.agenda.events.length, 1);
  assert.equal(migrated.pages[0].widgets[0].type, 'events');
  assert.equal(migrated.pages[0].widgets[1].type, 'calendar');
  assert.equal(createWidget('device', migrated.pages[0])?.w, 2);
});

test('actualiza los widgets visuales a iconos sin etiquetas por defecto', () => {
  const legacy = createDefaultProject();
  legacy.schemaVersion = 3;
  legacy.pages[0].widgets = [
    { id: 'tiempo', type: 'weather', x: 0, y: 0, w: 2, h: 2, config: { showLocation: true, title: 'Tiempo' } },
    { id: 'estado', type: 'device', x: 3, y: 0, w: 2, h: 2, config: { showLabels: true, showNetworkName: true } },
  ];
  const migrated = sanitizeProject(legacy);
  assert.equal(migrated.schemaVersion, 7);
  assert.equal(migrated.pages[0].widgets[0].config.showLocation, false);
  assert.equal(migrated.pages[0].widgets[1].config.showLabels, false);
  assert.equal(migrated.pages[0].widgets[1].config.showNetworkName, true);
  assert.equal(migrated.pages[0].widgets[1].config.showBattery, false);
});

test('conserva en el proyecto la caché validada de festivos españoles', () => {
  const project = createDefaultProject();
  project.agenda.holidays.spain = {
    enabled: true,
    region: 'ES-CT',
    cached: {
      year: 2026,
      region: 'ES-CT',
      fetchedAt: '2026-11-07T18:00:00.000Z',
      holidays: [{ id: 'es-2026-01-01', date: '2026-01-01', title: 'Año Nuevo', scope: 'Nacional' }],
    },
  };
  const clean = sanitizeProject(project);
  assert.equal(clean.schemaVersion, 7);
  assert.equal(clean.agenda.holidays.spain.cached.holidays.length, 1);
  assert.equal(clean.agenda.holidays.spain.cached.region, 'ES-CT');
});

test('planifica hora al minuto, tiempo a la hora y fecha/calendario a medianoche', () => {
  const now = new Date(2026, 10, 8, 10, 23, 45, 500);
  const page = { widgets: [{ type: 'clock', config: { showTime: true, showDate: true } }] };
  let schedule = dynamicScheduleForPage(page, now);
  assert.deepEqual([schedule.at.getHours(), schedule.at.getMinutes(), schedule.at.getSeconds()], [10, 24, 0]);
  assert.deepEqual(schedule.reasons, ['hora']);

  page.widgets = [{ type: 'clock', config: { showTime: false, showDate: true, showWeekday: false } }];
  schedule = dynamicScheduleForPage(page, now);
  assert.deepEqual([schedule.at.getHours(), schedule.at.getMinutes()], [0, 0]);
  assert.equal(schedule.at.getDate(), 9);

  page.widgets = [{ type: 'weather', config: {} }, { type: 'calendar', config: {} }];
  schedule = dynamicScheduleForPage(page, now);
  assert.deepEqual([schedule.at.getHours(), schedule.at.getMinutes()], [11, 0]);
  assert.deepEqual(schedule.reasons, ['tiempo']);
});

test('un reloj sin hora, fecha ni día no programa refrescos', () => {
  assert.equal(dynamicScheduleForPage({ widgets: [{ type: 'clock', config: { showTime: false, showDate: false, showWeekday: false } }] }), null);
});
