export const SCHEMA_VERSION = 7;
export const DISPLAY_WIDTH = 800;
export const DISPLAY_HEIGHT = 480;
export const GRID_COLUMNS = 20;
export const GRID_ROWS = 12;

export const WIDGET_TYPES = Object.freeze(['text', 'clock', 'weather', 'calendar', 'events', 'device', 'image', 'qr']);
export const BUTTON_ACTIONS = Object.freeze([
  'previous_page',
  'next_page',
  'show_page',
  'refresh',
  'toggle_invert',
  'blank',
  'none',
]);

function nextBoundary(now, unit) {
  const value = new Date(now);
  value.setSeconds(0, 0);
  if (unit === 'minute') value.setMinutes(value.getMinutes() + 1);
  if (unit === 'hour') { value.setMinutes(0); value.setHours(value.getHours() + 1); }
  if (unit === 'day') value.setHours(24, 0, 0, 0);
  return value;
}

export function dynamicScheduleForPage(page, now = new Date()) {
  const candidates = [];
  for (const widget of page?.widgets || []) {
    if (widget.type === 'clock') {
      if (widget.config?.showTime !== false) candidates.push({ at: nextBoundary(now, 'minute'), reason: 'hora' });
      else if (widget.config?.showDate !== false || widget.config?.showWeekday !== false) candidates.push({ at: nextBoundary(now, 'day'), reason: 'fecha' });
    } else if (widget.type === 'weather') candidates.push({ at: nextBoundary(now, 'hour'), reason: 'tiempo' });
    else if (widget.type === 'calendar' || widget.type === 'events') candidates.push({ at: nextBoundary(now, 'day'), reason: widget.type === 'calendar' ? 'calendario' : 'eventos' });
  }
  if (!candidates.length) return null;
  const earliest = Math.min(...candidates.map((candidate) => candidate.at.getTime()));
  return { at: new Date(earliest), reasons: [...new Set(candidates.filter((candidate) => candidate.at.getTime() === earliest).map((candidate) => candidate.reason))] };
}

const WIDGET_DEFAULTS = Object.freeze({
  text: { w: 2, h: 2, config: { text: 'Escribe algo', fontSize: 'auto', align: 'center', verticalAlign: 'middle', weight: 700, padding: 12, lineHeight: 1.08, border: false, invert: false } },
  clock: { w: 2, h: 2, config: { format: '24', showTime: true, showDate: true, showWeekday: true, dateStyle: 'short', locale: 'es-ES', timezone: '', padding: 12, border: false, invert: false } },
  weather: { w: 2, h: 2, config: { location: '', latitude: null, longitude: null, units: 'celsius', view: 'current', title: 'Tiempo', forecastDays: 3, showTitle: false, showLocation: false, showWind: false, showFeelsLike: false, showHumidity: false, showPrecipitation: false, showCloudCover: false, showSun: false, showIcon: true, showLabels: false, padding: 12, border: true, invert: false, weather: null } },
  calendar: { w: 2, h: 2, config: { view: 'month', weekMode: 'full', referenceDate: '', showHeader: true, showWeekNumbers: false, showEventMarkers: true, showEventLabels: false, showHolidayLabels: true, padding: 12, border: true, invert: false } },
  events: { w: 2, h: 2, config: { view: 'agenda', referenceDate: '', rangeDays: 14, maxEvents: 6, showTimes: true, dayStart: 8, dayEnd: 20, interval: 60, padding: 12, border: true, invert: false } },
  device: { w: 2, h: 2, config: { mode: 'compact', showBattery: false, showNetwork: true, showRefresh: true, showUptime: true, showFirmware: true, showTemperature: false, showErrors: true, showLabels: false, showNetworkName: true, padding: 12, border: true, invert: false } },
  image: { w: 2, h: 2, config: { assetId: null, fit: 'cover', focusX: 50, focusY: 50, rotation: 0, brightness: 0, contrast: 0, gamma: 1, threshold: 160, dither: 'floyd', invert: false, border: false } },
  qr: { w: 2, h: 2, config: { value: 'https://github.com/angelcubo01/mi-eink', correction: 'M', margin: 2, label: '', padding: 8, border: false, invert: false } },
});

function id(prefix) {
  const token = globalThis.crypto?.randomUUID?.() || `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
  return `${prefix}-${token}`;
}

export function deepClone(value) {
  return JSON.parse(JSON.stringify(value));
}

export function createPage(name = 'Página 1') {
  return { id: id('page'), name, background: 'white', frameId: null, widgets: [] };
}

export function createDefaultProject() {
  const page = createPage();
  return {
    schemaVersion: SCHEMA_VERSION,
    name: 'Mi panel',
    activePageId: page.id,
    pages: [page],
    assets: {},
    agenda: {
      events: [],
      holidays: { manual: [], spain: { enabled: false, region: '', cached: null } },
    },
    device: {
      display: { width: DISPLAY_WIDTH, height: DISPLAY_HEIGHT, rotation: 0, invert: false, threshold: 160, safeMargin: 0, refreshMode: 'full' },
      network: { mode: 'ap_sta', apSsid: 'MiEInk', homeSsid: '' },
      buttons: {
        key1: { action: 'previous_page', pageId: null },
        key2: { action: 'next_page', pageId: null },
        key3: { action: 'refresh', pageId: null },
      },
    },
  };
}

export function rectsOverlap(a, b) {
  return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

export function isInsideGrid(rect) {
  return rect.x >= 0 && rect.y >= 0 && rect.w >= 1 && rect.h >= 1 && rect.x + rect.w <= GRID_COLUMNS && rect.y + rect.h <= GRID_ROWS;
}

export function canPlace(page, rect, ignoreId = null) {
  return isInsideGrid(rect) && !page.widgets.some((widget) => widget.id !== ignoreId && rectsOverlap(widget, rect));
}

export function findFreePosition(page, width, height) {
  for (let y = 0; y <= GRID_ROWS - height; y += 1) {
    for (let x = 0; x <= GRID_COLUMNS - width; x += 1) {
      if (canPlace(page, { x, y, w: width, h: height })) return { x, y };
    }
  }
  return null;
}

export function createWidget(type, page, overrides = {}) {
  if (!WIDGET_TYPES.includes(type)) throw new Error(`Widget desconocido: ${type}`);
  const defaults = WIDGET_DEFAULTS[type];
  const width = Math.min(overrides.w || defaults.w, GRID_COLUMNS);
  const height = Math.min(overrides.h || defaults.h, GRID_ROWS);
  const free = overrides.x == null || overrides.y == null ? findFreePosition(page, width, height) : { x: overrides.x, y: overrides.y };
  if (!free || !canPlace(page, { ...free, w: width, h: height })) return null;
  return {
    id: id('widget'),
    type,
    x: free.x,
    y: free.y,
    w: width,
    h: height,
    locked: false,
    config: { ...deepClone(defaults.config), ...(overrides.config || {}) },
  };
}

export function addPage(project, sourcePageId = null) {
  const number = project.pages.length + 1;
  const source = sourcePageId ? project.pages.find((page) => page.id === sourcePageId) : null;
  const page = source ? deepClone(source) : createPage(`Página ${number}`);
  page.id = id('page');
  page.name = source ? `${source.name} copia` : `Página ${number}`;
  page.widgets = (page.widgets || []).map((widget) => ({ ...widget, id: id('widget') }));
  project.pages.push(page);
  project.activePageId = page.id;
  return page;
}

export function deletePage(project, pageId) {
  if (project.pages.length === 1) return false;
  const index = project.pages.findIndex((page) => page.id === pageId);
  if (index < 0) return false;
  project.pages.splice(index, 1);
  if (project.activePageId === pageId) project.activePageId = project.pages[Math.min(index, project.pages.length - 1)].id;
  for (const button of Object.values(project.device.buttons)) {
    if (button.pageId === pageId) button.pageId = null;
  }
  return true;
}

export function movePage(project, pageId, direction) {
  const index = project.pages.findIndex((page) => page.id === pageId);
  const target = index + direction;
  if (index < 0 || target < 0 || target >= project.pages.length) return false;
  [project.pages[index], project.pages[target]] = [project.pages[target], project.pages[index]];
  return true;
}

export function applyButtonAction(project, key, runtime = {}) {
  const button = project.device.buttons[key];
  const state = { blank: Boolean(runtime.blank), inverted: Boolean(runtime.inverted), refreshed: false };
  if (!button) return state;
  const index = project.pages.findIndex((page) => page.id === project.activePageId);
  if (button.action === 'previous_page') { project.activePageId = project.pages[(index - 1 + project.pages.length) % project.pages.length].id; state.blank = false; }
  if (button.action === 'next_page') { project.activePageId = project.pages[(index + 1) % project.pages.length].id; state.blank = false; }
  if (button.action === 'show_page' && project.pages.some((page) => page.id === button.pageId)) { project.activePageId = button.pageId; state.blank = false; }
  if (button.action === 'refresh') state.refreshed = true;
  if (button.action === 'toggle_invert') state.inverted = !state.inverted;
  if (button.action === 'blank') state.blank = true;
  return state;
}

function cleanNetwork(network = {}) {
  return {
    mode: network.mode === 'ap_sta' ? 'ap_sta' : 'ap_sta',
    apSsid: String(network.apSsid || 'MiEInk').slice(0, 32),
    homeSsid: String(network.homeSsid || '').slice(0, 32),
  };
}

function normaliseEvent(event = {}) {
  const date = /^\d{4}-\d{2}-\d{2}$/.test(String(event.date || '')) ? String(event.date) : new Date().toISOString().slice(0, 10);
  const time = (value) => /^\d{2}:\d{2}$/.test(String(value || '')) ? String(value) : '';
  return {
    id: String(event.id || id('event')),
    date,
    start: time(event.start || event.time),
    end: time(event.end),
    title: String(event.title || 'Evento').slice(0, 100),
    allDay: Boolean(event.allDay || (!event.start && !event.time && !event.end)),
  };
}

function normaliseHoliday(holiday = {}) {
  return {
    id: String(holiday.id || id('holiday')),
    date: /^\d{4}-\d{2}-\d{2}$/.test(String(holiday.date || '')) ? String(holiday.date) : new Date().toISOString().slice(0, 10),
    title: String(holiday.title || holiday.name || 'Festivo').slice(0, 100),
    scope: String(holiday.scope || 'Local').slice(0, 50),
  };
}

function uniqueEvents(events) {
  const seen = new Set();
  return events.filter((event) => {
    const key = [event.date, event.start, event.end, event.title, event.allDay].join('|').toLowerCase();
    if (seen.has(key)) return false;
    seen.add(key);
    return true;
  });
}

export function sanitizeProject(input) {
  const fallback = createDefaultProject();
  if (!input || typeof input !== 'object') return fallback;
  const project = deepClone(input);
  const sourceVersion = Number(project.schemaVersion || 1);
  project.schemaVersion = SCHEMA_VERSION;
  project.name = String(project.name || 'Mi panel').slice(0, 80);
  project.pages = Array.isArray(project.pages) && project.pages.length ? project.pages : fallback.pages;
  const inheritedEvents = [];
  project.pages = project.pages.map((page, pageIndex) => {
    const widgets = Array.isArray(page.widgets) ? page.widgets.filter((widget) => WIDGET_TYPES.includes(widget.type)).map((widget) => {
      if (sourceVersion < 3 && widget.type === 'calendar' && Array.isArray(widget.config?.events)) inheritedEvents.push(...widget.config.events);
      const migrated = sourceVersion < 2 ? {
        ...widget,
        x: Math.round(Number(widget.x || 0) * GRID_COLUMNS / 16),
        y: Math.round(Number(widget.y || 0) * GRID_ROWS / 10),
        w: Math.max(1, Math.round(Number(widget.w || 1) * GRID_COLUMNS / 16)),
        h: Math.max(1, Math.round(Number(widget.h || 1) * GRID_ROWS / 10)),
      } : widget;
      const legacyEventView = sourceVersion < 3 && migrated.type === 'calendar' && ['agenda', 'day'].includes(migrated.config?.view);
      const width = Math.max(1, Math.min(GRID_COLUMNS, Number(migrated.w)));
      const height = Math.max(1, Math.min(GRID_ROWS, Number(migrated.h)));
      return ({
        id: String(migrated.id || id('widget')),
        type: legacyEventView ? 'events' : migrated.type,
        x: Math.max(0, Math.min(GRID_COLUMNS - width, Number(migrated.x))),
        y: Math.max(0, Math.min(GRID_ROWS - height, Number(migrated.y))),
        w: width,
        h: height,
        locked: Boolean(migrated.locked),
        config: (() => {
          const config = migrated.config && typeof migrated.config === 'object' ? { ...migrated.config } : {};
          if (sourceVersion < 3) delete config.events;
          if (legacyEventView) {
            config.view = migrated.config?.view === 'day' ? 'day' : 'agenda';
            config.rangeDays ??= 14;
            config.dayStart ??= 8;
            config.dayEnd ??= 20;
            config.interval ??= 60;
          }
          if (sourceVersion < 3 && migrated.type === 'calendar' && !legacyEventView) {
            config.view = migrated.config?.view === 'week' ? 'week' : 'month';
            config.weekMode ??= 'full';
          }
          if (sourceVersion < 4 && migrated.type === 'weather') {
            config.showTitle = false;
            config.showLocation = false;
            config.showLabels = false;
          }
          if (sourceVersion < 4 && migrated.type === 'device') {
            config.showLabels = false;
            config.showNetworkName = false;
          }
          if (sourceVersion < 5 && migrated.type === 'device') {
            config.showBattery = false;
            config.showNetworkName = true;
            config.showUptime = true;
            config.showFirmware = true;
          }
          return config;
        })(),
      });
    }).filter((widget) => isInsideGrid(widget)) : [];
    const collisionFree = [];
    for (const widget of widgets) if (!collisionFree.some((placed) => rectsOverlap(placed, widget))) collisionFree.push(widget);
    return {
      id: String(page.id || id('page')),
      name: String(page.name || `Página ${pageIndex + 1}`).slice(0, 50),
      background: page.background === 'black' ? 'black' : 'white',
      frameId: /^[a-f0-9]{64}$/i.test(String(page.frameId || '')) ? String(page.frameId).toLowerCase() : null,
      widgets: collisionFree,
    };
  });
  project.activePageId = project.pages.some((page) => page.id === project.activePageId) ? project.activePageId : project.pages[0].id;
  const inputAssets = project.assets && typeof project.assets === 'object' ? project.assets : {};
  project.assets = Object.fromEntries(Object.entries(inputAssets).flatMap(([assetId, asset]) => {
    const idValue = String(asset?.id || assetId).toLowerCase();
    if (!/^[a-f0-9]{64}$/.test(idValue)) return [];
    return [[idValue, {
      id: idValue,
      name: String(asset?.name || 'imagen').slice(0, 120),
      mime: 'image/bmp',
      format: 'mono1',
      width: Math.max(1, Math.min(DISPLAY_WIDTH, Number(asset?.width || DISPLAY_WIDTH))),
      height: Math.max(1, Math.min(DISPLAY_HEIGHT, Number(asset?.height || DISPLAY_HEIGHT))),
      size: Math.max(0, Number(asset?.size || 0)),
      crc32: String(asset?.crc32 || '').toLowerCase(),
      url: `/api/assets/${idValue}`,
    }]];
  }));
  const priorAgenda = project.agenda && typeof project.agenda === 'object' ? project.agenda : {};
  const priorHolidays = priorAgenda.holidays && typeof priorAgenda.holidays === 'object' ? priorAgenda.holidays : {};
  project.agenda = {
    events: uniqueEvents([...(Array.isArray(priorAgenda.events) ? priorAgenda.events : []), ...inheritedEvents].map(normaliseEvent)),
    holidays: {
      manual: (Array.isArray(priorHolidays.manual) ? priorHolidays.manual : []).map(normaliseHoliday),
      spain: {
        enabled: Boolean(priorHolidays.spain?.enabled),
        region: /^ES-[A-Z]{2}$/.test(String(priorHolidays.spain?.region || '')) ? String(priorHolidays.spain.region) : '',
        cached: (() => {
          const cached = priorHolidays.spain?.cached;
          if (!cached || !Number.isInteger(Number(cached.year)) || !Array.isArray(cached.holidays)) return null;
          const region = /^ES-[A-Z]{2}$/.test(String(cached.region || '')) ? String(cached.region) : '';
          return {
            year: Number(cached.year),
            region,
            fetchedAt: String(cached.fetchedAt || ''),
            holidays: cached.holidays.map(normaliseHoliday),
          };
        })(),
      },
    },
  };
  project.device = project.device && typeof project.device === 'object' ? project.device : fallback.device;
  const display = project.device.display || {};
  project.device.display = {
    ...fallback.device.display,
    ...display,
    refreshMode: 'full',
    width: DISPLAY_WIDTH,
    height: DISPLAY_HEIGHT,
  };
  delete project.device.display.refresh;
  delete project.device.display.refreshMinutes;
  project.device.network = cleanNetwork(project.device.network);
  project.device.buttons = { ...fallback.device.buttons, ...(project.device.buttons || {}) };
  for (const key of ['key1', 'key2', 'key3']) {
    const current = project.device.buttons[key] || fallback.device.buttons[key];
    project.device.buttons[key] = {
      action: BUTTON_ACTIONS.includes(current.action) ? current.action : fallback.device.buttons[key].action,
      pageId: current.pageId || null,
    };
  }
  return project;
}
