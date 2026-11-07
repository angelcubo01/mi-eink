import { DISPLAY_HEIGHT, DISPLAY_WIDTH, GRID_COLUMNS, GRID_ROWS } from './model.js';
import { drawMaterialIcon } from './material-icons.js';

export const MARGINS = Object.freeze({ top: 48, right: 32, bottom: 48, left: 32 });
export const BMP_SIZE = 48062;

const FONT_FAMILY = 'Arial, Helvetica, sans-serif';
const CELL_WIDTH = DISPLAY_WIDTH / GRID_COLUMNS;
const CELL_HEIGHT = DISPLAY_HEIGHT / GRID_ROWS;

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
    } else current = candidate;
  }
  if (current) parts.push(current);
  return parts.length ? parts : [''];
}

export function wrapText(ctx, text, maxWidth) {
  const normalized = String(text || '').replace(/\r\n?/g, '\n').trim();
  if (!normalized) return [];
  const result = [];
  for (const paragraph of normalized.split('\n')) {
    const cleaned = paragraph.trim().replace(/\s+/g, ' ');
    if (!cleaned) {
      result.push('');
      continue;
    }
    let line = '';
    for (const word of cleaned.split(' ')) {
      const pieces = ctx.measureText(word).width > maxWidth ? splitLongToken(ctx, word, maxWidth) : [word];
      for (const piece of pieces) {
        const candidate = line ? `${line} ${piece}` : piece;
        if (line && ctx.measureText(candidate).width > maxWidth) {
          result.push(line);
          line = piece;
        } else line = candidate;
      }
    }
    result.push(line);
  }
  return result;
}

function layoutAtSize(ctx, text, fontSize, width, height, weight = 700, lineHeightScale = 1.08) {
  ctx.font = `${weight} ${fontSize}px ${FONT_FAMILY}`;
  const lines = wrapText(ctx, text, width);
  const lineHeight = Math.ceil(fontSize * lineHeightScale);
  const measuredHeight = lines.length * lineHeight;
  const measuredWidth = lines.reduce((largest, line) => Math.max(largest, ctx.measureText(line).width), 0);
  return { fontSize, lineHeight, lines, width: measuredWidth, height: measuredHeight, fits: measuredWidth <= width && measuredHeight <= height };
}

export function calculateLayout(ctx, text, width = DISPLAY_WIDTH - MARGINS.left - MARGINS.right, height = DISPLAY_HEIGHT - MARGINS.top - MARGINS.bottom, weight = 700, lineHeightScale = 1.08) {
  if (!String(text || '').trim()) return { fontSize: 0, lineHeight: 0, lines: [], width: 0, height: 0, fits: true };
  let low = 8;
  let high = Math.max(8, Math.min(360, height));
  let best = layoutAtSize(ctx, text, 8, width, height, weight, lineHeightScale);
  while (low <= high) {
    const middle = Math.floor((low + high) / 2);
    const candidate = layoutAtSize(ctx, text, middle, width, height, weight, lineHeightScale);
    if (candidate.fits) {
      best = candidate;
      low = middle + 1;
    } else high = middle - 1;
  }
  return best;
}

function widgetRect(widget) {
  return { x: widget.x * CELL_WIDTH, y: widget.y * CELL_HEIGHT, width: widget.w * CELL_WIDTH, height: widget.h * CELL_HEIGHT };
}

function frame(ctx, rect, config) {
  const inverted = Boolean(config.invert);
  ctx.fillStyle = inverted ? '#000' : '#fff';
  ctx.fillRect(rect.x, rect.y, rect.width, rect.height);
  if (config.border) {
    ctx.strokeStyle = inverted ? '#fff' : '#000';
    ctx.lineWidth = 2;
    ctx.strokeRect(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2);
  }
  return { foreground: inverted ? '#fff' : '#000', background: inverted ? '#000' : '#fff' };
}

function drawFittedText(ctx, text, rect, config = {}) {
  const padding = Math.max(0, Number(config.padding ?? 12));
  const weight = Number(config.weight || 700);
  const lineHeightScale = Math.max(0.8, Math.min(2, Number(config.lineHeight || 1.08)));
  const availableWidth = Math.max(1, rect.width - padding * 2);
  const availableHeight = Math.max(1, rect.height - padding * 2);
  const requested = config.fontSize === 'auto' || !Number(config.fontSize) ? null : Number(config.fontSize);
  const layout = requested ? layoutAtSize(ctx, text, requested, availableWidth, availableHeight, weight, lineHeightScale) : calculateLayout(ctx, text, availableWidth, availableHeight, weight, lineHeightScale);
  ctx.font = `${weight} ${layout.fontSize}px ${FONT_FAMILY}`;
  ctx.textBaseline = 'middle';
  ctx.textAlign = config.align || 'center';
  const x = config.align === 'left' ? rect.x + padding : config.align === 'right' ? rect.x + rect.width - padding : rect.x + rect.width / 2;
  const blockHeight = layout.lines.length * layout.lineHeight;
  let firstY = rect.y + rect.height / 2 - blockHeight / 2 + layout.lineHeight / 2;
  if (config.verticalAlign === 'top') firstY = rect.y + padding + layout.lineHeight / 2;
  if (config.verticalAlign === 'bottom') firstY = rect.y + rect.height - padding - blockHeight + layout.lineHeight / 2;
  layout.lines.forEach((line, index) => ctx.fillText(line, x, firstY + index * layout.lineHeight, availableWidth));
}

function drawClock(ctx, rect, config, now) {
  const options = config.timezone ? { timeZone: config.timezone } : {};
  const time = new Intl.DateTimeFormat(config.locale || 'es-ES', { ...options, hour: '2-digit', minute: '2-digit', hour12: config.format === '12' }).format(now);
  const dateOptions = config.dateStyle === 'long'
    ? { ...options, day: 'numeric', month: 'long', year: 'numeric' }
    : { ...options, day: '2-digit', month: 'short' };
  const weekday = new Intl.DateTimeFormat(config.locale || 'es-ES', { ...options, weekday: 'long' }).format(now);
  const lines = [config.showTime !== false ? time : '', config.showWeekday !== false ? weekday : '', config.showDate !== false ? new Intl.DateTimeFormat(config.locale || 'es-ES', dateOptions).format(now) : ''].filter(Boolean).join('\n');
  drawFittedText(ctx, lines, rect, { ...config, weight: 700, lineHeight: 1.02 });
}

export function weatherIconName(code, isDay = 1) {
  const value = Number(code);
  if (value >= 200 && value <= 232) return 'thunderstorm';
  if (value >= 300 && value <= 321) return 'rain_light';
  if (value >= 500 && value <= 531) return 'rain';
  if (value >= 600 && value <= 622) return 'snow';
  if ([701, 711, 721, 731, 741, 751, 761, 762].includes(value)) return value === 701 ? 'mist' : 'fog';
  if (value === 771) return 'wind';
  if (value === 781) return 'tornado';
  if (value === 800) return isDay ? 'sunny' : 'night';
  if (value === 801) return isDay ? 'weather' : 'partly_cloudy_night';
  if (value >= 802 && value <= 804) return 'cloud';
  return 'weather';
}

function drawWeatherIcon(ctx, code, x, y, size, isDay = 1) {
  drawMaterialIcon(ctx, weatherIconName(code, isDay), x, y, size);
}

function drawDataIcon(ctx, kind, x, y, size, value = null) {
  const names = { rain: 'precipitation', sun: 'sunny', uv: 'sunny', uptime: 'clock' };
  drawMaterialIcon(ctx, names[kind] || kind, x, y, size);
}

function drawMetricGrid(ctx, rect, metrics, config = {}) {
  const visible = metrics.filter((metric) => metric && metric.value != null && metric.value !== '');
  if (!visible.length) return;
  const columns = visible.length === 1 || rect.width < 170 ? 1 : 2;
  const rows = Math.ceil(visible.length / columns);
  const cellWidth = rect.width / columns;
  const cellHeight = rect.height / rows;
  visible.forEach((metric, index) => {
    const column = index % columns; const row = Math.floor(index / columns);
    const x = rect.x + column * cellWidth; const y = rect.y + row * cellHeight;
    const iconSize = Math.max(15, Math.min(30, cellHeight * .52, cellWidth * .22));
    drawDataIcon(ctx, metric.icon, x + iconSize * .65, y + cellHeight / 2, iconSize, metric.level);
    ctx.font = `700 ${Math.max(9, Math.min(20, cellHeight * (config.showLabels ? .32 : .43)))}px ${FONT_FAMILY}`;
    ctx.textAlign = 'left'; ctx.textBaseline = config.showLabels ? 'middle' : 'middle';
    ctx.fillText(String(metric.value), x + iconSize * 1.35, y + cellHeight * (config.showLabels ? .38 : .52), cellWidth - iconSize * 1.55);
    if (config.showLabels && metric.label) { ctx.font = `700 ${Math.max(7, Math.min(10, cellHeight * .18))}px ${FONT_FAMILY}`; ctx.fillText(metric.label, x + iconSize * 1.35, y + cellHeight * .69, cellWidth - iconSize * 1.55); }
  });
}

function drawWeather(ctx, rect, config) {
  const weather = config.weather;
  if (!weather) {
    drawWeatherIcon(ctx, null, rect.x + rect.width / 2, rect.y + rect.height / 2, Math.min(rect.width, rect.height) * .42);
    return;
  }
  const units = weather.units || { temperature: config.units === 'fahrenheit' ? '°F' : '°C', wind: config.units === 'fahrenheit' ? 'mph' : 'km/h', precipitation: config.units === 'fahrenheit' ? 'in' : 'mm' };
  const unit = units.temperature;
  if (config.view === 'forecast' && Array.isArray(weather.daily)) {
    const days = weather.daily.slice(0, Math.max(1, Math.min(7, Number(config.forecastDays || 3))));
    const padding = Math.max(4, Number(config.padding ?? 12));
    const header = config.showTitle ? Math.max(20, rect.height * .12) : 0;
    if (header) { ctx.font = `700 ${Math.max(10, Math.min(17, header * .52))}px ${FONT_FAMILY}`; ctx.textAlign = 'left'; ctx.textBaseline = 'middle'; ctx.fillText(config.title || 'TIEMPO', rect.x + padding, rect.y + header / 2); }
    const area = { x: rect.x + padding, y: rect.y + header + padding * .3, width: rect.width - padding * 2, height: rect.height - header - padding * 1.3 };
    const width = area.width / days.length;
    days.forEach((day, index) => {
      const x = area.x + index * width;
      ctx.font = `700 ${Math.max(7, Math.min(11, area.height * .13))}px ${FONT_FAMILY}`; ctx.textAlign = 'center'; ctx.textBaseline = 'top'; ctx.fillText(String(day.label || '').slice(0, 3).toUpperCase(), x + width / 2, area.y, width - 2);
      if (config.showIcon !== false) drawWeatherIcon(ctx, day.code, x + width / 2, area.y + area.height * .42, Math.min(width, area.height) * .42);
      ctx.font = `700 ${Math.max(8, Math.min(15, area.height * .16))}px ${FONT_FAMILY}`; ctx.textBaseline = 'middle'; ctx.fillText(`${Math.round(day.min)}–${Math.round(day.max)}°`, x + width / 2, area.y + area.height * .76, width - 2);
      const detail = config.showPrecipitation && day.precipitationProbability != null ? `${Math.round(day.precipitationProbability)}%` : config.showWind && day.wind != null ? `${Math.round(day.wind)}${units.wind}` : config.showSun && day.sunrise ? new Date(day.sunrise).toLocaleTimeString('es-ES', { hour: '2-digit', minute: '2-digit' }) : '';
      if (detail) { const kind = config.showPrecipitation ? 'rain' : config.showWind ? 'wind' : 'sunrise'; drawDataIcon(ctx, kind, x + width * .34, area.y + area.height * .93, Math.max(10, area.height * .13)); ctx.font = `700 ${Math.max(7, Math.min(10, area.height * .11))}px ${FONT_FAMILY}`; ctx.textAlign = 'left'; ctx.fillText(detail, x + width * .48, area.y + area.height * .93, width * .48); }
    });
  } else {
    const padding = Number(config.padding ?? 12);
    const titleHeight = config.showTitle ? Math.max(18, rect.height * .12) : 0;
    if (titleHeight) { ctx.font = `700 ${Math.max(10, Math.min(17, titleHeight * .58))}px ${FONT_FAMILY}`; ctx.textAlign = 'left'; ctx.textBaseline = 'middle'; ctx.fillText(config.title || 'TIEMPO', rect.x + padding, rect.y + titleHeight / 2); }
    const iconSize = config.showIcon === false ? 0 : Math.min(rect.width, rect.height - titleHeight) * .38;
    const area = { x: rect.x + padding, y: rect.y + titleHeight + padding * .25, width: rect.width - padding * 2, height: rect.height - titleHeight - padding * 1.25 };
    if (iconSize) drawWeatherIcon(ctx, weather.code, area.x + iconSize / 2, area.y + area.height / 2, iconSize, weather.isDay);
    const metrics = [
      { icon: 'temperature', value: `${Math.round(weather.temperature)}${unit}`, label: 'Temperatura' },
      config.showFeelsLike && weather.apparentTemperature != null ? { icon: 'feels', value: `${Math.round(weather.apparentTemperature)}${unit}`, label: 'Sensación' } : null,
      config.showHumidity && weather.humidity != null ? { icon: 'humidity', value: `${Math.round(weather.humidity)}%`, label: 'Humedad' } : null,
      config.showWind && weather.wind != null ? { icon: 'wind', value: `${Math.round(weather.wind)} ${units.wind}`, label: 'Viento' } : null,
      config.showPrecipitation && weather.precipitation != null ? { icon: 'rain', value: `${weather.precipitation}${units.precipitation}`, label: 'Lluvia' } : null,
      config.showCloudCover && weather.cloudCover != null ? { icon: 'cloud', value: `${Math.round(weather.cloudCover)}%`, label: 'Nubes' } : null,
      config.showSun && weather.daily?.[0]?.sunrise ? { icon: 'sunrise', value: new Date(weather.daily[0].sunrise).toLocaleTimeString('es-ES', { hour: '2-digit', minute: '2-digit' }), label: 'Amanecer' } : null,
      config.showSun && weather.daily?.[0]?.sunset ? { icon: 'sunrise', value: new Date(weather.daily[0].sunset).toLocaleTimeString('es-ES', { hour: '2-digit', minute: '2-digit' }), label: 'Atardecer' } : null,
      config.showLocation && config.location ? { icon: 'location', value: config.location.split(',')[0], label: 'Lugar' } : null,
    ];
    const metricRect = iconSize ? { x: area.x + iconSize + padding * .6, y: area.y, width: area.width - iconSize - padding * .6, height: area.height } : area;
    drawMetricGrid(ctx, metricRect, metrics, config);
  }
}

function referenceDate(value, fallback) {
  return /^\d{4}-\d{2}-\d{2}$/.test(String(value || '')) ? new Date(`${value}T12:00:00`) : fallback;
}

function dateKey(date) { return new Date(date).toISOString().slice(0, 10); }
function monday(date) { const result = new Date(date); result.setHours(12, 0, 0, 0); result.setDate(result.getDate() - ((result.getDay() + 6) % 7)); return result; }
function eventsForDay(events, day) { return events.filter((event) => event.date === dateKey(day)).sort((a, b) => String(a.start || '').localeCompare(String(b.start || ''))); }
function holidayForDay(holidays, day) { return holidays.find((holiday) => holiday.date === dateKey(day)) || null; }

function isoWeekNumber(date) {
  const value = new Date(Date.UTC(date.getFullYear(), date.getMonth(), date.getDate()));
  value.setUTCDate(value.getUTCDate() + 4 - (value.getUTCDay() || 7));
  const yearStart = new Date(Date.UTC(value.getUTCFullYear(), 0, 1));
  return Math.ceil((((value - yearStart) / 86400000) + 1) / 7);
}

function drawHolidayMarker(ctx, x, y, size) {
  ctx.fillRect(x, y, size, size);
  ctx.globalAlpha = .45; ctx.fillRect(x + size + 2, y, Math.max(2, Math.floor(size / 2)), size); ctx.globalAlpha = 1;
}

function drawCalendar(ctx, rect, config, agenda, now) {
  const padding = Math.max(4, Number(config.padding ?? 12));
  const events = agenda?.events || [];
  const holidays = agenda?.holidays || [];
  const current = referenceDate(config.referenceDate, now);
  if (rect.width < 180 || rect.height < 130) return drawFittedText(ctx, config.view === 'week' ? 'Semana\nAmplía este calendario' : 'Calendario\nAmplía este widget', rect, { ...config, weight: 700 });
  const content = { x: rect.x + padding, y: rect.y + padding, width: Math.max(1, rect.width - padding * 2), height: Math.max(1, rect.height - padding * 2) };
  const title = config.view === 'week' ? `SEMANA · ${new Intl.DateTimeFormat('es-ES', { day: 'numeric', month: 'short' }).format(monday(current))}` : new Intl.DateTimeFormat('es-ES', { month: 'long', year: 'numeric' }).format(current).toUpperCase();
  const headerHeight = config.showHeader === false ? 0 : Math.max(24, content.height * .14);
  if (headerHeight) { ctx.font = `700 ${Math.max(12, Math.min(23, headerHeight * .45))}px ${FONT_FAMILY}`; ctx.textAlign = 'left'; ctx.textBaseline = 'middle'; ctx.fillText(title, content.x, content.y + headerHeight / 2); }
  const names = ['L', 'M', 'X', 'J', 'V', 'S', 'D'];
  const weekDays = config.view === 'week' && config.weekMode === 'work' ? names.slice(0, 5) : names;
  const gridY = content.y + headerHeight; const gridHeight = content.height - headerHeight; const columns = weekDays.length;
  const start = config.view === 'week' ? monday(current) : new Date(current.getFullYear(), current.getMonth(), 1, 12);
  const offset = config.view === 'week' ? 0 : (start.getDay() + 6) % 7;
  const totalDays = config.view === 'week' ? columns : new Date(current.getFullYear(), current.getMonth() + 1, 0).getDate();
  const rows = config.view === 'week' ? 1 : Math.ceil((offset + totalDays) / 7);
  const labelHeight = Math.max(16, gridHeight * .12); const weekNumberWidth = config.showWeekNumbers ? Math.max(18, content.width * .055) : 0; const gridX = content.x + weekNumberWidth; const cellWidth = (content.width - weekNumberWidth) / columns; const cellHeight = (gridHeight - labelHeight) / rows;
  ctx.font = `700 ${Math.max(9, Math.min(15, labelHeight * .58))}px ${FONT_FAMILY}`; ctx.textAlign = 'center'; weekDays.forEach((name, index) => ctx.fillText(name, gridX + cellWidth * (index + .5), gridY + labelHeight / 2));
  if (config.showWeekNumbers) for (let row = 0; row < rows; row += 1) { const rowDate = new Date(start.getFullYear(), start.getMonth(), start.getDate() + (config.view === 'week' ? 0 : row * 7 - offset), 12); ctx.font = `700 ${Math.max(7, Math.min(11, cellHeight * .18))}px ${FONT_FAMILY}`; ctx.textAlign = 'center'; ctx.textBaseline = 'middle'; ctx.fillText(String(isoWeekNumber(rowDate)), content.x + weekNumberWidth / 2, gridY + labelHeight + row * cellHeight + cellHeight / 2); }
  for (let row = 0; row < rows; row += 1) for (let column = 0; column < columns; column += 1) {
    const slot = row * 7 + column; const dayNumber = config.view === 'week' ? column + 1 : slot - offset + 1;
    // En la vista semanal el lunes ocupa la columna cero. La validación de
    // huecos solo corresponde a la vista mensual.
    if (config.view !== 'week' && (dayNumber < 1 || dayNumber > totalDays)) continue;
    const day = config.view === 'week' ? new Date(start.getFullYear(), start.getMonth(), start.getDate() + column, 12) : new Date(current.getFullYear(), current.getMonth(), dayNumber, 12);
    const cell = { x: gridX + column * cellWidth, y: gridY + labelHeight + row * cellHeight, width: cellWidth, height: cellHeight };
    const holiday = holidayForDay(holidays, day); ctx.strokeRect(cell.x, cell.y, cell.width, cell.height);
    ctx.textAlign = 'left'; ctx.textBaseline = 'top'; ctx.font = `700 ${Math.max(9, Math.min(17, cellHeight * .22))}px ${FONT_FAMILY}`; const dateText = String(day.getDate()); ctx.fillText(dateText, cell.x + 4, cell.y + 3);
    if (holiday) drawHolidayMarker(ctx, cell.x + 7 + ctx.measureText(dateText).width, cell.y + 6, Math.max(4, Math.min(8, cellHeight * .1)));
    const dayEvents = eventsForDay(events, day);
    if (holiday && config.showHolidayLabels !== false) { ctx.font = `700 ${Math.max(7, Math.min(11, cellHeight * .14))}px ${FONT_FAMILY}`; ctx.fillText(holiday.title.slice(0, Math.max(5, Math.floor(cellWidth / 7))), cell.x + 4, cell.y + cellHeight * .42, cell.width - 8); }
    if (config.showEventLabels && dayEvents.length) { ctx.font = `700 ${Math.max(7, Math.min(11, cellHeight * .14))}px ${FONT_FAMILY}`; ctx.fillText(dayEvents[0].title.slice(0, Math.max(5, Math.floor(cellWidth / 7))), cell.x + 4, cell.y + cellHeight * (holiday ? .64 : .45), cell.width - 8); }
    if (config.showEventMarkers !== false && dayEvents.length) ctx.fillRect(cell.x + 4, cell.y + cellHeight - 7, Math.max(5, Math.min(cell.width - 8, 5 + dayEvents.length * 5)), 3);
  }
}

function drawEvents(ctx, rect, config, agenda, now) {
  const current = referenceDate(config.referenceDate, now);
  const events = [...(agenda?.events || [])].sort((a, b) => `${a.date} ${a.start || ''}`.localeCompare(`${b.date} ${b.start || ''}`));
  if (config.view !== 'day') {
    const until = new Date(current); until.setDate(until.getDate() + Math.max(1, Number(config.rangeDays || 14)));
    const visible = events.filter((event) => new Date(`${event.date}T12:00:00`) >= current && new Date(`${event.date}T12:00:00`) <= until).slice(0, Math.max(1, Number(config.maxEvents || 6)));
    const lines = visible.map((event) => `${new Intl.DateTimeFormat('es-ES', { day: '2-digit', month: 'short' }).format(new Date(`${event.date}T12:00:00`))} ${event.allDay || !event.start ? '' : `${event.start} `}${event.title}`);
    return drawFittedText(ctx, lines.length ? `AGENDA\n${lines.join('\n')}` : 'AGENDA\nSin próximos eventos', rect, { ...config, align: 'left', verticalAlign: 'top', weight: 700, lineHeight: 1.28 });
  }
  const start = Math.max(0, Math.min(23, Number(config.dayStart ?? 8))); const end = Math.max(start + 1, Math.min(24, Number(config.dayEnd ?? 20))); const interval = [30, 60, 120].includes(Number(config.interval)) ? Number(config.interval) : 60;
  const dayEvents = eventsForDay(events, current); const padding = Math.max(5, Number(config.padding ?? 12)); const heading = 28; const timeWidth = Math.max(34, rect.width * .17);
  const content = { x: rect.x + padding, y: rect.y + heading, width: rect.width - padding * 2, height: rect.height - heading - padding };
  ctx.font = `700 ${Math.max(11, Math.min(17, heading * .55))}px ${FONT_FAMILY}`; ctx.textAlign = 'left'; ctx.textBaseline = 'middle'; ctx.fillText(new Intl.DateTimeFormat('es-ES', { weekday: 'long', day: 'numeric', month: 'short' }).format(current).toUpperCase(), content.x, rect.y + heading / 2);
  const slots = (end - start) * 60 / interval; const slotHeight = content.height / slots;
  for (let index = 0; index < slots; index += 1) { const minutes = start * 60 + index * interval; const y = content.y + index * slotHeight; ctx.globalAlpha = .42; ctx.beginPath(); ctx.moveTo(content.x + timeWidth, y); ctx.lineTo(content.x + content.width, y); ctx.stroke(); ctx.globalAlpha = 1; ctx.font = `700 ${Math.max(7, Math.min(11, slotHeight * .45))}px ${FONT_FAMILY}`; ctx.textBaseline = 'top'; ctx.fillText(`${String(Math.floor(minutes / 60)).padStart(2, '0')}:${String(minutes % 60).padStart(2, '0')}`, content.x, y + 2); }
  dayEvents.forEach((event) => { if (event.allDay || !event.start) return; const [hour, minute] = event.start.split(':').map(Number); const [endHour, endMinute] = (event.end || event.start).split(':').map(Number); const from = hour * 60 + minute; const to = Math.max(from + interval, endHour * 60 + endMinute); if (to <= start * 60 || from >= end * 60) return; const y = content.y + Math.max(0, from - start * 60) / interval * slotHeight; const height = Math.max(slotHeight, Math.min(content.height - (y - content.y), (to - from) / interval * slotHeight)); ctx.strokeRect(content.x + timeWidth + 3, y + 1, content.width - timeWidth - 5, height - 2); ctx.font = `700 ${Math.max(7, Math.min(13, height * .35))}px ${FONT_FAMILY}`; ctx.textBaseline = 'middle'; ctx.fillText(event.title, content.x + timeWidth + 7, y + height / 2, content.width - timeWidth - 12); });
  const allDay = dayEvents.filter((event) => event.allDay || !event.start); if (allDay.length) { ctx.font = `700 9px ${FONT_FAMILY}`; ctx.textBaseline = 'bottom'; ctx.fillText(allDay.map((event) => event.title).join(' · '), content.x + timeWidth, rect.y + rect.height - 3, content.width - timeWidth); }
}

function drawDevice(ctx, rect, config, telemetry) {
  const padding = Math.max(5, Number(config.padding ?? 12));
  if (!telemetry) { drawDataIcon(ctx, 'screen', rect.x + rect.width / 2, rect.y + rect.height / 2, Math.min(rect.width, rect.height) * .4); return; }
  const metrics = [
    config.showBattery !== false ? { icon: 'battery', value: `${telemetry.power?.percentage ?? '—'}%`, label: telemetry.power?.charging ? 'Cargando' : 'Batería', level: telemetry.power?.percentage } : null,
    config.showNetwork !== false ? { icon: 'wifi', value: config.showNetworkName || telemetry.network?.rssi == null ? telemetry.network?.ssid || 'AP' : `${telemetry.network.rssi} dBm`, label: 'Wi‑Fi' } : null,
    config.showRefresh !== false ? { icon: 'refresh', value: `${telemetry.display?.refreshCount ?? 0}`, label: telemetry.display?.refreshMinutes ? `${telemetry.display.refreshMinutes} min` : 'Manual' } : null,
    config.showUptime ? { icon: 'uptime', value: telemetry.system?.uptimeLabel || '—', label: 'Activa' } : null,
    config.showFirmware ? { icon: 'chip', value: telemetry.system?.firmware || '—', label: 'Firmware' } : null,
    config.showTemperature && telemetry.system?.temperatureC != null ? { icon: 'temperature', value: `${Math.round(telemetry.system.temperatureC)}°`, label: 'Temperatura' } : null,
    config.showErrors && telemetry.error ? { icon: 'alert', value: telemetry.error, label: 'Aviso' } : null,
  ];
  drawMetricGrid(ctx, { x: rect.x + padding, y: rect.y + padding, width: rect.width - padding * 2, height: rect.height - padding * 2 }, metrics, { showLabels: config.showLabels, detailed: config.mode === 'detailed' });
}

function loadImage(url) {
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => resolve(image);
    image.onerror = reject;
    image.src = url;
  });
}

function createScratchCanvas(width, height) {
  if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(width, height);
  const canvas = document.createElement('canvas');
  canvas.width = width; canvas.height = height;
  return canvas;
}

export function ditherImageData(data, width, height, config = {}) {
  const work = new Float32Array(width * height);
  const brightness = Number(config.brightness || 0) * 2.55;
  const contrast = Number(config.contrast || 0);
  const contrastFactor = contrast === 0 ? 1 : (259 * (contrast + 255)) / (255 * (259 - contrast));
  const gamma = Math.max(.1, Number(config.gamma || 1));
  for (let pixel = 0, offset = 0; pixel < work.length; pixel += 1, offset += 4) {
    let value = data[offset] * .299 + data[offset + 1] * .587 + data[offset + 2] * .114;
    value = contrastFactor * (value - 128) + 128 + brightness;
    work[pixel] = Math.max(0, Math.min(255, 255 * Math.pow(Math.max(0, Math.min(255, value)) / 255, 1 / gamma)));
  }
  const threshold = Number(config.threshold || 160);
  const algorithm = config.dither || 'floyd';
  const bayer = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];
  for (let y = 0; y < height; y += 1) for (let x = 0; x < width; x += 1) {
    const pixel = y * width + x;
    const localThreshold = algorithm === 'bayer' ? threshold + (bayer[(y % 4) * 4 + (x % 4)] - 7.5) * 10 : threshold;
    const value = work[pixel] < localThreshold ? 0 : 255;
    const error = work[pixel] - value;
    work[pixel] = value;
    if (algorithm === 'floyd') {
      if (x + 1 < width) work[pixel + 1] += error * 7 / 16;
      if (y + 1 < height && x > 0) work[pixel + width - 1] += error * 3 / 16;
      if (y + 1 < height) work[pixel + width] += error * 5 / 16;
      if (y + 1 < height && x + 1 < width) work[pixel + width + 1] += error / 16;
    }
  }
  for (let pixel = 0, offset = 0; pixel < work.length; pixel += 1, offset += 4) {
    const value = config.invert ? 255 - work[pixel] : work[pixel];
    data[offset] = data[offset + 1] = data[offset + 2] = value;
    data[offset + 3] = 255;
  }
  return data;
}

async function drawImageWidget(ctx, rect, config, assetResolver) {
  if (!config.assetId) {
    drawFittedText(ctx, 'Añade una imagen', rect, { ...config, weight: 700 });
    return;
  }
  try {
    const image = await loadImage(assetResolver(config.assetId));
    const width = Math.max(1, Math.round(rect.width));
    const height = Math.max(1, Math.round(rect.height));
    const scratch = createScratchCanvas(width, height);
    const scratchCtx = scratch.getContext('2d', { willReadFrequently: true });
    scratchCtx.fillStyle = config.invert ? '#000' : '#fff';
    scratchCtx.fillRect(0, 0, width, height);
    scratchCtx.imageSmoothingEnabled = true;
    scratchCtx.imageSmoothingQuality = 'high';
    const scale = config.fit === 'contain' ? Math.min(width / image.width, height / image.height) : Math.max(width / image.width, height / image.height);
    const drawWidth = image.width * scale;
    const drawHeight = image.height * scale;
    const focusX = Math.max(0, Math.min(100, Number(config.focusX ?? 50))) / 100;
    const focusY = Math.max(0, Math.min(100, Number(config.focusY ?? 50))) / 100;
    scratchCtx.save();
    scratchCtx.translate(width / 2, height / 2);
    scratchCtx.rotate(Number(config.rotation || 0) * Math.PI / 180);
    scratchCtx.drawImage(image, -drawWidth * focusX, -drawHeight * focusY, drawWidth, drawHeight);
    scratchCtx.restore();
    const region = scratchCtx.getImageData(0, 0, width, height);
    ditherImageData(region.data, width, height, config);
    scratchCtx.putImageData(region, 0, 0);
    ctx.drawImage(scratch, Math.round(rect.x), Math.round(rect.y));
  } catch {
    drawFittedText(ctx, 'Imagen no disponible', rect, { ...config, weight: 700 });
  }
}

function drawQr(ctx, rect, config, foreground, background) {
  if (typeof globalThis.qrcode !== 'function') {
    drawFittedText(ctx, 'QR no disponible', rect, { ...config, weight: 700 });
    return;
  }
  try {
    const qr = globalThis.qrcode(0, config.correction || 'M');
    qr.addData(String(config.value || ''));
    qr.make();
    const count = qr.getModuleCount();
    const margin = Number(config.margin ?? 2);
    const labelHeight = config.label ? 28 : 0;
    const moduleSize = Math.max(1, Math.floor(Math.min(rect.width, rect.height - labelHeight) / (count + margin * 2)));
    const size = moduleSize * (count + margin * 2);
    const startX = Math.floor(rect.x + (rect.width - size) / 2);
    const startY = Math.floor(rect.y + (rect.height - labelHeight - size) / 2);
    ctx.fillStyle = background;
    ctx.fillRect(startX, startY, size, size);
    ctx.fillStyle = foreground;
    for (let row = 0; row < count; row += 1) {
      for (let col = 0; col < count; col += 1) {
        if (qr.isDark(row, col)) ctx.fillRect(startX + (col + margin) * moduleSize, startY + (row + margin) * moduleSize, moduleSize, moduleSize);
      }
    }
    if (config.label) {
      ctx.font = `700 16px ${FONT_FAMILY}`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'bottom';
      ctx.fillText(String(config.label).slice(0, 40), rect.x + rect.width / 2, rect.y + rect.height - 5, rect.width - 10);
    }
  } catch {
    drawFittedText(ctx, 'Contenido QR demasiado largo', rect, { ...config, weight: 700 });
  }
}

function monochrome(ctx, threshold, dither = false) {
  const image = ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  const data = image.data;
  if (!dither) {
    for (let index = 0; index < data.length; index += 4) {
      const value = data[index] * 0.299 + data[index + 1] * 0.587 + data[index + 2] * 0.114 < threshold ? 0 : 255;
      data[index] = data[index + 1] = data[index + 2] = value;
    }
  } else {
    const luminance = new Float32Array(DISPLAY_WIDTH * DISPLAY_HEIGHT);
    for (let pixel = 0, offset = 0; pixel < luminance.length; pixel += 1, offset += 4) luminance[pixel] = data[offset] * 0.299 + data[offset + 1] * 0.587 + data[offset + 2] * 0.114;
    for (let y = 0; y < DISPLAY_HEIGHT; y += 1) {
      for (let x = 0; x < DISPLAY_WIDTH; x += 1) {
        const pixel = y * DISPLAY_WIDTH + x;
        const old = luminance[pixel];
        const value = old < threshold ? 0 : 255;
        const error = old - value;
        luminance[pixel] = value;
        if (x + 1 < DISPLAY_WIDTH) luminance[pixel + 1] += error * 7 / 16;
        if (y + 1 < DISPLAY_HEIGHT && x > 0) luminance[pixel + DISPLAY_WIDTH - 1] += error * 3 / 16;
        if (y + 1 < DISPLAY_HEIGHT) luminance[pixel + DISPLAY_WIDTH] += error * 5 / 16;
        if (y + 1 < DISPLAY_HEIGHT && x + 1 < DISPLAY_WIDTH) luminance[pixel + DISPLAY_WIDTH + 1] += error / 16;
      }
    }
    for (let pixel = 0, offset = 0; pixel < luminance.length; pixel += 1, offset += 4) data[offset] = data[offset + 1] = data[offset + 2] = luminance[pixel] < 128 ? 0 : 255;
  }
  ctx.putImageData(image, 0, 0);
}

export async function renderPage(canvas, project, page, options = {}) {
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  ctx.save();
  ctx.fillStyle = page.background === 'black' ? '#000' : '#fff';
  ctx.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  for (const widget of page.widgets) {
    const rect = widgetRect(widget);
    ctx.save();
    ctx.beginPath();
    ctx.rect(rect.x, rect.y, rect.width, rect.height);
    ctx.clip();
    const colors = frame(ctx, rect, widget.config || {});
    ctx.fillStyle = colors.foreground;
    if (widget.type === 'text') drawFittedText(ctx, widget.config.text, rect, widget.config);
    if (widget.type === 'clock') drawClock(ctx, rect, widget.config, options.now || new Date());
    if (widget.type === 'weather') drawWeather(ctx, rect, widget.config);
    if (widget.type === 'calendar') drawCalendar(ctx, rect, widget.config, options.agenda || project.agenda, options.now || new Date());
    if (widget.type === 'events') drawEvents(ctx, rect, widget.config, options.agenda || project.agenda, options.now || new Date());
    if (widget.type === 'device') drawDevice(ctx, rect, widget.config, options.telemetry || null);
    if (widget.type === 'image') await drawImageWidget(ctx, rect, widget.config, options.assetResolver || ((assetId) => `/api/assets/${assetId}`));
    if (widget.type === 'qr') drawQr(ctx, rect, widget.config, colors.foreground, colors.background);
    ctx.restore();
  }
  ctx.restore();
  monochrome(ctx, Number(project.device.display.threshold || 160), false);
  if (project.device.display.invert || options.runtimeInvert) {
    const image = ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    for (let index = 0; index < image.data.length; index += 4) image.data[index] = image.data[index + 1] = image.data[index + 2] = 255 - image.data[index];
    ctx.putImageData(image, 0, 0);
  }
  if (options.blank) {
    ctx.fillStyle = '#fff';
    ctx.fillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  }
  if (Number(project.device.display.rotation) === 180) {
    const copy = ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    const source = new Uint32Array(copy.data.buffer);
    const rotated = new ImageData(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    const target = new Uint32Array(rotated.data.buffer);
    for (let index = 0; index < source.length; index += 1) target[source.length - 1 - index] = source[index];
    ctx.putImageData(rotated, 0, 0);
  }
  return canvas;
}

function writeU16(view, offset, value) { view.setUint16(offset, value, true); }
function writeU32(view, offset, value) { view.setUint32(offset, value, true); }

export function canvasToMonoBmp(canvas, threshold = 160) {
  const rowBytes = DISPLAY_WIDTH / 8;
  const pixelBytes = rowBytes * DISPLAY_HEIGHT;
  const buffer = new ArrayBuffer(62 + pixelBytes);
  const bytes = new Uint8Array(buffer);
  const view = new DataView(buffer);
  bytes[0] = 0x42; bytes[1] = 0x4d;
  writeU32(view, 2, BMP_SIZE); writeU32(view, 10, 62); writeU32(view, 14, 40);
  writeU32(view, 18, DISPLAY_WIDTH); writeU32(view, 22, DISPLAY_HEIGHT);
  writeU16(view, 26, 1); writeU16(view, 28, 1); writeU32(view, 34, pixelBytes);
  writeU32(view, 38, 2835); writeU32(view, 42, 2835); writeU32(view, 46, 2); writeU32(view, 50, 2);
  bytes.set([0, 0, 0, 0, 255, 255, 255, 0], 54);
  const pixels = canvas.getContext('2d', { willReadFrequently: true }).getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT).data;
  for (let outputY = 0; outputY < DISPLAY_HEIGHT; outputY += 1) {
    const sourceY = DISPLAY_HEIGHT - 1 - outputY;
    for (let x = 0; x < DISPLAY_WIDTH; x += 1) {
      const source = (sourceY * DISPLAY_WIDTH + x) * 4;
      const luminance = pixels[source] * 0.299 + pixels[source + 1] * 0.587 + pixels[source + 2] * 0.114;
      if (luminance >= threshold) bytes[62 + outputY * rowBytes + Math.floor(x / 8)] |= 1 << (7 - (x % 8));
    }
  }
  return bytes;
}
