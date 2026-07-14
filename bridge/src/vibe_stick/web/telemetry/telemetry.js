const MODE_STORAGE_KEY = "vibe-stick-telemetry-mode";
const FIXTURE_KEY = "fixture";
const REFRESH_MS = 2000;
const DEFAULT_API_ROOT = "/telemetry/v1";
const STALE_AFTER_SECONDS = 75;
const SERIES_COLORS = ["#77c4ff", "#f0be57", "#68d39a", "#f48a62", "#d7d16a"];
const FIXTURES = {
  industrial: {
    devices: [
      {
        id: "alpha",
        name: "VibeStick A",
        model: "Battery puck",
        online: true,
        stale: false,
        usb_connected: true,
        charging: false,
        voltage_v: 4.08,
        estimated_percent: 76,
        latest_sample_at: "2026-07-13T09:59:30Z",
        last_seen_at: "2026-07-13T09:59:30Z",
      },
      {
        id: "beta",
        name: "VibeStick B",
        model: "Spare pack",
        online: false,
        stale: true,
        usb_connected: false,
        charging: false,
        voltage_v: 3.63,
        estimated_percent: 21,
        latest_sample_at: "2026-07-13T08:51:00Z",
        last_seen_at: "2026-07-13T08:51:00Z",
      },
    ],
    sessions: [
      {
        id: "sess-20260713-a",
        device_id: "alpha",
        label: "Charge cycle",
        started_at: "2026-07-13T09:12:00Z",
        ended_at: "2026-07-13T09:59:30Z",
        sample_count: 8,
      },
      {
        id: "sess-20260712-b",
        device_id: "alpha",
        label: "Desk idle",
        started_at: "2026-07-12T18:05:00Z",
        ended_at: "2026-07-12T19:01:00Z",
        sample_count: 7,
      },
      {
        id: "sess-20260711-c",
        device_id: "beta",
        label: "Field test",
        started_at: "2026-07-11T16:20:00Z",
        ended_at: "2026-07-11T17:02:00Z",
        sample_count: 6,
      },
    ],
    sessionDetails: {
      "sess-20260713-a": {
        id: "sess-20260713-a",
        device_id: "alpha",
        label: "Charge cycle",
        started_at: "2026-07-13T09:12:00Z",
        ended_at: "2026-07-13T09:59:30Z",
        sample_count: 8,
      },
      "sess-20260712-b": {
        id: "sess-20260712-b",
        device_id: "alpha",
        label: "Desk idle",
        started_at: "2026-07-12T18:05:00Z",
        ended_at: "2026-07-12T19:01:00Z",
        sample_count: 7,
      },
      "sess-20260711-c": {
        id: "sess-20260711-c",
        device_id: "beta",
        label: "Field test",
        started_at: "2026-07-11T16:20:00Z",
        ended_at: "2026-07-11T17:02:00Z",
        sample_count: 6,
      },
    },
    samples: {
      "sess-20260713-a": [
        { elapsed_s: 0, voltage_v: 4.18, percent: 100 },
        { elapsed_s: 300, voltage_v: 4.12, percent: 94 },
        { elapsed_s: 600, voltage_v: 4.07, percent: 88 },
        { elapsed_s: 900, voltage_v: 4.00, percent: 81 },
        { elapsed_s: 1200, voltage_v: 3.96, percent: 74 },
        { elapsed_s: 1500, voltage_v: 3.91, percent: 67 },
        { elapsed_s: 1800, voltage_v: 3.86, percent: 59 },
        { elapsed_s: 2250, voltage_v: 3.81, percent: 51 },
      ],
      "sess-20260712-b": [
        { elapsed_s: 0, voltage_v: 4.11, percent: 92 },
        { elapsed_s: 480, voltage_v: 4.08, percent: 89 },
        { elapsed_s: 960, voltage_v: 4.05, percent: 86 },
        { elapsed_s: 1680, voltage_v: 4.01, percent: 81 },
        { elapsed_s: 2280, voltage_v: 3.96, percent: 74 },
        { elapsed_s: 2880, voltage_v: 3.92, percent: 68 },
        { elapsed_s: 3360, voltage_v: 3.88, percent: 61 },
      ],
      "sess-20260711-c": [
        { elapsed_s: 0, voltage_v: 4.05, percent: 85 },
        { elapsed_s: 360, voltage_v: 3.99, percent: 79 },
        { elapsed_s: 900, voltage_v: 3.93, percent: 69 },
        { elapsed_s: 1200, voltage_v: 3.87, percent: 59 },
        { elapsed_s: 1680, voltage_v: 3.81, percent: 48 },
        { elapsed_s: 2460, voltage_v: 3.74, percent: 35 },
      ],
    },
  },
};

const state = {
  apiRoot: DEFAULT_API_ROOT,
  fixtureKey: "",
  fixture: null,
  devices: [],
  sessions: [],
  selectedDeviceId: "",
  selectedSessionIds: [],
  selectedMode: "voltage",
  sessionCache: new Map(),
  refreshing: false,
  lastRefreshAt: "",
  lastError: "",
};

const dom = {};

init().catch((error) => {
  console.error(error);
  setStatus("Offline", "Unable to load telemetry");
  renderError(error);
});

async function init() {
  cacheDom();
  state.fixtureKey = new URLSearchParams(window.location.search).get(FIXTURE_KEY) || "";
  state.apiRoot = resolveApiRoot();
  state.fixture = state.fixtureKey ? FIXTURES[state.fixtureKey] || null : null;
  state.selectedMode = readSavedMode();
  applyMode(state.selectedMode);
  bindEvents();
  setFixtureIndicator(Boolean(state.fixture));
  await refreshAll();
  window.setInterval(() => {
    refreshAll({ quiet: true });
  }, REFRESH_MS);
}

function cacheDom() {
  dom.connectionState = document.getElementById("connection-state");
  dom.refreshState = document.getElementById("refresh-state");
  dom.fixtureState = document.getElementById("fixture-state");
  dom.deviceMeta = document.getElementById("device-meta");
  dom.deviceList = document.getElementById("device-list");
  dom.sessionMeta = document.getElementById("session-meta");
  dom.sessionList = document.getElementById("session-list");
  dom.summary = document.getElementById("summary");
  dom.chart = document.getElementById("telemetry-chart");
  dom.legend = document.getElementById("legend");
  dom.csvLink = document.getElementById("csv-link");
  dom.modeButtons = Array.from(document.querySelectorAll("[data-mode]"));
}

function bindEvents() {
  dom.modeButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const mode = button.dataset.mode;
      if (!mode || mode === state.selectedMode) {
        return;
      }
      applyMode(mode);
      saveMode(mode);
      render();
    });
  });
}

async function refreshAll({ quiet = false } = {}) {
  if (state.refreshing) {
    return;
  }
  state.refreshing = true;
  if (!quiet) {
    setStatus("Live", "Refreshing telemetry");
  }
  try {
    const [devicesPayload, sessionsPayload] = await Promise.all([
      fetchJson("/devices"),
      fetchJson("/sessions"),
    ]);
    state.devices = normalizeList(devicesPayload, "devices").map(normalizeDevice);
    state.sessions = normalizeList(sessionsPayload, "sessions").map(normalizeSession);
    ensureSelection();
    await refreshSelectedSessions();
    state.lastRefreshAt = formatClock(new Date());
    state.lastError = "";
    setStatus("Live", `Updated ${state.lastRefreshAt}`);
    render();
  } catch (error) {
    state.lastError = error instanceof Error ? error.message : String(error);
    setStatus("Degraded", state.lastError);
    renderError(error);
  } finally {
    state.refreshing = false;
  }
}

async function refreshSelectedSessions() {
  const wanted = new Set(state.selectedSessionIds);
  const nextCache = new Map();
  await Promise.all(
    [...wanted].map(async (sessionId) => {
      const [detailPayload, samplesPayload] = await Promise.all([
        fetchJson(`/sessions/${encodeURIComponent(sessionId)}`),
        fetchJson(`/sessions/${encodeURIComponent(sessionId)}/samples`),
      ]);
      const detail = normalizeSession(detailPayload?.session || detailPayload, sessionId);
      const samples = normalizeSamples(samplesPayload, detail);
      nextCache.set(sessionId, { detail, samples });
    }),
  );
  state.sessionCache = nextCache;
}

async function fetchJson(path) {
  if (state.fixture) {
    return mockFetch(path);
  }
  const response = await fetch(`${state.apiRoot}${path}`, {
    headers: {
      Accept: "application/json",
    },
    cache: "no-store",
  });
  if (!response.ok) {
    throw new Error(`Request failed: ${response.status} ${response.statusText}`);
  }
  return response.json();
}

function mockFetch(path) {
  const fixture = state.fixture;
  if (!fixture) {
    throw new Error("Fixture mode is unavailable.");
  }
  const cleanPath = path.replace(/^\/+/, "");
  if (cleanPath === "devices") {
    return deepClone({ devices: fixture.devices });
  }
  if (cleanPath === "sessions") {
    return deepClone({ sessions: fixture.sessions });
  }
  const sessionMatch = cleanPath.match(/^sessions\/([^/]+)(?:\/(samples|export\.csv))?$/);
  if (!sessionMatch) {
    throw new Error(`Unknown fixture path: ${path}`);
  }
  const sessionId = decodeURIComponent(sessionMatch[1]);
  const tail = sessionMatch[2] || "";
  if (tail === "samples") {
    return deepClone({ samples: fixture.samples[sessionId] || [] });
  }
  if (tail === "export.csv") {
    return buildCsvResponse(sessionId);
  }
  return deepClone(fixture.sessionDetails[sessionId] || { id: sessionId });
}

function buildCsvResponse(sessionId) {
  const session = state.sessionCache.get(sessionId)?.detail || normalizeSession(state.fixture.sessionDetails[sessionId] || {}, sessionId);
  const rows = ["elapsed_s,voltage_v,percent"];
  const samples = state.sessionCache.get(sessionId)?.samples || normalizeSamples({ samples: state.fixture.samples[sessionId] || [] }, session);
  samples.forEach((sample) => {
    rows.push([sample.elapsedSeconds, sample.voltage, sample.percent ?? ""].join(","));
  });
  return Promise.resolve({
    ok: true,
    json: async () => ({ exported: true, session_id: sessionId }),
    text: async () => rows.join("\n") + "\n",
  });
}

function resolveApiRoot() {
  const params = new URLSearchParams(window.location.search);
  const explicit = params.get("apiBase");
  if (explicit) {
    return explicit.replace(/\/$/, "");
  }
  const url = new URL(window.location.href);
  const pathname = url.pathname || "/";
  if (pathname.endsWith("/telemetry") || pathname.endsWith("/telemetry/")) {
    return "/telemetry/v1";
  }
  const basePath = pathname.endsWith("/") ? pathname : pathname.replace(/[^/]*$/, "/");
  const trimmed = basePath.endsWith("/") ? basePath.slice(0, -1) : basePath;
  return trimmed ? `${trimmed}/v1` : DEFAULT_API_ROOT;
}

function normalizeList(payload, key) {
  if (Array.isArray(payload)) {
    return payload;
  }
  if (payload && Array.isArray(payload[key])) {
    return payload[key];
  }
  return [];
}

function normalizeDevice(raw) {
  const now = Date.now();
  const lastSeenRaw = pick(raw, ["last_seen_at", "latest_sample_at", "updated_at", "seen_at", "timestamp"]);
  const lastSeen = parseDate(lastSeenRaw);
  const ageSeconds = lastSeen ? Math.max(0, Math.round((now - lastSeen.getTime()) / 1000)) : null;
  const stale = toBoolean(pick(raw, ["stale", "is_stale"])) || (ageSeconds !== null && ageSeconds > STALE_AFTER_SECONDS);
  const voltageMv = toNumber(pick(raw, ["voltage_mv", "battery_mv"]));
  const voltage =
    voltageMv !== null
      ? voltageMv / 1000
      : toNumber(pick(raw, ["voltage_v", "battery_voltage", "voltage", "latest_voltage"]));
  const latestPercent = toNumber(pick(raw, ["estimated_percent", "percent", "battery_percent", "latest_percent"]));
  const charging = toBoolean(pick(raw, ["charging", "is_charging"]));
  const usb = toBoolean(pick(raw, ["usb_powered", "usb_connected", "usb", "powered_by_usb", "charging_usb"]));
  const online = toBoolean(pick(raw, ["online", "connected", "present"])) || !stale;
  return {
    id: String(pick(raw, ["id", "device_id", "serial", "uuid"]) || ""),
    name: String(pick(raw, ["name", "label", "display_name", "device_id", "model"]) || "Unnamed device"),
    model: String(pick(raw, ["board", "model", "kind", "type"]) || ""),
    online,
    stale,
    usb,
    charging,
    voltage,
    latestPercent,
    lastSeen,
    ageSeconds,
    activeSessionId: String(pick(raw, ["active_session_id", "session_id"]) || ""),
    raw,
  };
}

function normalizeSession(raw, fallbackId = "") {
  const summary = raw?.summary && typeof raw.summary === "object" ? raw.summary : {};
  const startedAt = parseDate(pick(raw, ["started_at", "start_at", "start_time", "created_at"]));
  const endedAt = parseDate(pick(raw, ["ended_at", "end_at", "end_time", "stopped_at"]));
  const sampleCount =
    toInteger(pick(raw, ["sample_count", "samples_count", "count"])) ??
    toInteger(summary.sample_count) ??
    0;
  return {
    id: String(pick(raw, ["id", "session_id", "uuid"]) || fallbackId),
    deviceId: String(pick(raw, ["device_id", "device", "source_device_id"]) || ""),
    label: String(pick(raw, ["label", "name", "title", "kind"]) || "Session"),
    startedAt,
    endedAt,
    sampleCount,
    durationSeconds:
      toInteger(pick(raw, ["duration_s", "duration_seconds"])) ??
      toInteger(summary.duration_seconds) ??
      null,
    raw,
  };
}

function normalizeSamples(payload, session) {
  const samples = Array.isArray(payload) ? payload : Array.isArray(payload?.samples) ? payload.samples : [];
  const parsed = samples.map((sample, index) => {
    const elapsedSeconds = toNumber(pick(sample, ["elapsed_s", "elapsed_sec", "elapsedSeconds", "elapsed_seconds"]));
    const elapsedMs = toNumber(pick(sample, ["elapsed_ms"]));
    const elapsed =
      elapsedSeconds ??
      (elapsedMs !== null ? elapsedMs / 1000 : elapsedFromTimestamp(sample, session, index));
    const voltageMv = toNumber(pick(sample, ["voltage_mv", "battery_mv"]));
    return {
      index,
      elapsedSeconds: Number.isFinite(elapsed) ? Math.max(0, elapsed) : index,
      voltage:
        voltageMv !== null
          ? voltageMv / 1000
          : toNumber(pick(sample, ["voltage_v", "voltage", "battery_voltage", "v"])) ?? 0,
      percent: toNumber(pick(sample, ["percent", "battery_percent", "estimated_percent", "charge_percent"])),
      timestamp: parseDate(pick(sample, ["recorded_at", "timestamp", "created_at", "sampled_at"])),
      raw: sample,
    };
  });
  parsed.sort((a, b) => a.elapsedSeconds - b.elapsedSeconds);
  return parsed;
}

function elapsedFromTimestamp(sample, session, index) {
  const timestamp = parseDate(pick(sample, ["recorded_at", "timestamp", "created_at", "sampled_at"]));
  const start = session.startedAt || parseDate(session.raw?.started_at);
  if (timestamp && start) {
    return Math.max(0, (timestamp.getTime() - start.getTime()) / 1000);
  }
  return index;
}

function ensureSelection() {
  if (!state.devices.length) {
    state.selectedDeviceId = "";
    state.selectedSessionIds = [];
    return;
  }
  const selectedDevice = state.devices.find((device) => device.id === state.selectedDeviceId);
  if (!selectedDevice || !state.selectedDeviceId) {
    const preferred =
      state.devices.find((device) => device.online && !device.stale) ||
      state.devices.find((device) => device.online) ||
      state.devices[0];
    state.selectedDeviceId = preferred?.id || "";
  }

  const deviceSessions = sessionsForActiveDevice();
  const wanted = state.selectedSessionIds.filter((sessionId) => deviceSessions.some((session) => session.id === sessionId));
  if (wanted.length === 0) {
    state.selectedSessionIds = deviceSessions.slice(0, 2).map((session) => session.id);
  } else {
    state.selectedSessionIds = wanted;
  }
  if (!state.selectedSessionIds.length && deviceSessions.length) {
    state.selectedSessionIds = [deviceSessions[0].id];
  }
}

function sessionsForActiveDevice() {
  return [...state.sessions].sort((a, b) => {
    const left = b.startedAt?.getTime?.() || 0;
    const right = a.startedAt?.getTime?.() || 0;
    return left - right;
  });
}

function render() {
  renderDevices();
  renderSessions();
  renderChart();
  renderSummary();
  renderCsvLink();
}

function renderDevices() {
  dom.deviceMeta.textContent = `${state.devices.length} devices`;
  dom.deviceList.replaceChildren();
  state.devices.forEach((device) => {
    const item = document.createElement("article");
    item.className = `device-item${device.id === state.selectedDeviceId ? " is-active" : ""}`;
    item.setAttribute("role", "listitem");
    item.dataset.deviceId = device.id;

    const button = document.createElement("button");
    button.type = "button";
    button.className = "device-item__button";
    button.setAttribute("aria-pressed", device.id === state.selectedDeviceId ? "true" : "false");
    button.addEventListener("click", () => {
      if (state.selectedDeviceId === device.id) {
        return;
      }
      state.selectedDeviceId = device.id;
      state.selectedSessionIds = [];
      ensureSelection();
      refreshSelectedSessions().then(render).catch((error) => renderError(error));
      render();
    });

    const row = document.createElement("div");
    row.className = "device-item__row";

    const nameBlock = document.createElement("div");
    const name = document.createElement("div");
    name.className = "device-item__name";
    name.textContent = device.name;
    const subline = document.createElement("div");
    subline.className = "device-item__subline";
    subline.textContent = device.model || device.id || "device";
    nameBlock.append(name, subline);

    const badgeWrap = document.createElement("div");
    badgeWrap.className = "badge-row";
    badgeWrap.append(
      badgeFor(device.online ? "online" : "offline", device.online ? "good" : "danger"),
      badgeFor(device.stale ? "stale" : "fresh", device.stale ? "warn" : "good"),
    );
    if (device.usb) {
      badgeWrap.append(badgeFor("usb", "good"));
    }
    if (device.charging) {
      badgeWrap.append(badgeFor("charging", "warn"));
    }

    row.append(nameBlock, badgeWrap);

    const metrics = document.createElement("div");
    metrics.className = "metrics-grid";
    metrics.append(
      metric("Voltage", formatVoltage(device.voltage, false)),
      metric("Percent", formatPercent(device.latestPercent, false)),
      metric("Seen", formatAge(device.lastSeen, device.ageSeconds)),
      metric("State", device.stale ? "stale" : "live"),
    );

    button.append(row, metrics);
    item.append(button);
    dom.deviceList.append(item);
  });
}

function renderSessions() {
  const sessions = sessionsForActiveDevice();
  const selectedCount = state.selectedSessionIds.length;
  dom.sessionMeta.textContent = `${selectedCount} selected`;
  dom.sessionList.replaceChildren();
  sessions.forEach((session, index) => {
    const item = document.createElement("article");
    item.className = `session-item${state.selectedSessionIds.includes(session.id) ? " is-selected" : ""}`;
    item.setAttribute("role", "listitem");
    item.dataset.sessionId = session.id;

    const top = document.createElement("div");
    top.className = "session-item__row";

    const labelWrap = document.createElement("label");
    labelWrap.className = "session-item__check";

    const checkbox = document.createElement("input");
    checkbox.type = "checkbox";
    checkbox.checked = state.selectedSessionIds.includes(session.id);
    checkbox.addEventListener("change", () => {
      const next = new Set(state.selectedSessionIds);
      if (checkbox.checked) {
        next.add(session.id);
      } else {
        next.delete(session.id);
      }
      state.selectedSessionIds = [...next];
      if (!state.selectedSessionIds.length && sessions.length) {
        state.selectedSessionIds = [session.id];
        checkbox.checked = true;
      }
      refreshSelectedSessions().then(render).catch((error) => renderError(error));
      render();
    });

    const labelText = document.createElement("span");
    labelText.className = "session-item__name";
    labelText.textContent = session.label || session.id;
    labelWrap.append(checkbox, labelText);

    const badges = document.createElement("div");
    badges.className = "badge-row";
    if (index === 0) {
      badges.append(badgeFor("latest", "good"));
    }
    badges.append(badgeFor(session.deviceId || "global", ""));
    if (session.sampleCount) {
      badges.append(badgeFor(`${session.sampleCount} samples`, ""));
    }

    top.append(labelWrap, badges);

    const meta = document.createElement("div");
    meta.className = "session-item__meta";
    meta.innerHTML = [
      `start <span class="session-item__value">${formatTimestamp(session.startedAt)}</span>`,
      `end <span class="session-item__value">${formatTimestamp(session.endedAt)}</span>`,
      `duration <span class="session-item__value">${formatDuration(session.durationSeconds, session.startedAt, session.endedAt)}</span>`,
      `voltage <span class="session-item__value">${sessionVoltageSummary(session.id)}</span>`,
    ].join("<br>");

    const actions = document.createElement("div");
    actions.className = "session-item__actions";
    const exportLink = document.createElement("a");
    exportLink.className = "session-item__link";
    exportLink.href = `${state.apiRoot}/sessions/${encodeURIComponent(session.id)}/export.csv`;
    exportLink.textContent = "CSV";
    exportLink.setAttribute("download", "");
    actions.append(exportLink);

    item.append(top, meta, actions);
    dom.sessionList.append(item);
  });
}

function renderChart() {
  const sessions = selectedSessionsWithData();
  const mode = state.selectedMode;
  if (!sessions.length) {
    dom.chart.innerHTML = "";
    dom.legend.replaceChildren();
    return;
  }
  const plot = buildChart(sessions, mode);
  dom.chart.innerHTML = plot.svg;
  dom.legend.replaceChildren(...plot.legend);
}

function renderSummary() {
  const sessions = selectedSessionsWithData();
  const device = state.devices.find((entry) => entry.id === state.selectedDeviceId);
  if (!device) {
    dom.summary.textContent = "No device selected.";
    return;
  }
  const latest = latestTelemetry(device, sessions);
  const sessionNames = sessions.map((session) => session.detail.label || session.detail.id).join(", ");
  dom.summary.innerHTML = [
    `<strong>${escapeHtml(device.name)}</strong>`,
    `voltage ${escapeHtml(formatVoltage(latest.voltage, false))}`,
    `estimated ${escapeHtml(formatPercent(latest.percent, false))}`,
    `USB ${device.usb ? "on" : "off"}`,
    `charging ${device.charging ? "yes" : "no"}`,
    `sessions ${escapeHtml(sessionNames || "none")}`,
  ].join(" · ");
}

function renderCsvLink() {
  const primary = primarySelectedSession();
  if (!primary) {
    dom.csvLink.href = "#";
    dom.csvLink.setAttribute("aria-disabled", "true");
    return;
  }
  dom.csvLink.href = state.fixture
    ? buildCsvDataUrl(primary.id)
    : `${state.apiRoot}/sessions/${encodeURIComponent(primary.id)}/export.csv`;
  dom.csvLink.setAttribute("aria-disabled", "false");
}

function renderError(error) {
  const text = error instanceof Error ? error.message : String(error);
  dom.refreshState.textContent = text;
  dom.connectionState.textContent = "Degraded";
  dom.connectionState.classList.remove("status-chip--live");
  dom.connectionState.classList.add("status-chip--warn");
}

function selectedSessionsWithData() {
  return state.selectedSessionIds
    .map((sessionId) => {
      const cached = state.sessionCache.get(sessionId);
      if (!cached) {
        return null;
      }
      return cached;
    })
    .filter(Boolean);
}

function primarySelectedSession() {
  const first = state.selectedSessionIds[0];
  return first ? state.sessionCache.get(first)?.detail || null : null;
}

function sessionVoltageSummary(sessionId) {
  const cached = state.sessionCache.get(sessionId);
  if (!cached || !cached.samples.length) {
    return "n/a";
  }
  const first = cached.samples[0];
  const last = cached.samples[cached.samples.length - 1];
  return `${formatVoltage(first.voltage, false)} → ${formatVoltage(last.voltage, false)}`;
}

function latestTelemetry(device, sessions) {
  const latestSession = sessions[0];
  const sessionSample = latestSession?.samples?.[latestSession.samples.length - 1];
  const voltage = toNumber(device.voltage) ?? sessionSample?.voltage ?? 0;
  const percent = toNumber(device.latestPercent) ?? sessionSample?.percent ?? estimatePercentFromVoltage(voltage);
  return { voltage, percent };
}

function buildChart(sessions, mode) {
  const width = 960;
  const height = 360;
  const pad = { left: 64, right: 16, top: 20, bottom: 42 };
  const valueRange = mode === "percent" ? { min: 0, max: 100 } : deriveVoltageRange(sessions);
  const timeRange = deriveTimeRange(sessions);
  const rows = [];
  const legend = [];

  rows.push(
    `<rect x="0" y="0" width="${width}" height="${height}" fill="transparent"></rect>`,
    `<rect x="${pad.left}" y="${pad.top}" width="${width - pad.left - pad.right}" height="${height - pad.top - pad.bottom}" rx="8" ry="8" class="telemetry-band"></rect>`,
  );

  const gridLines = 6;
  for (let i = 0; i <= gridLines; i += 1) {
    const fraction = i / gridLines;
    const y = pad.top + (height - pad.top - pad.bottom) * fraction;
    rows.push(`<line x1="${pad.left}" x2="${width - pad.right}" y1="${y}" y2="${y}" class="telemetry-grid"></line>`);
    const value = valueRange.max - (valueRange.max - valueRange.min) * fraction;
    rows.push(
      `<text x="${pad.left - 10}" y="${y + 4}" text-anchor="end" class="telemetry-label">${escapeHtml(
        mode === "percent" ? `${Math.round(value)}%` : `${value.toFixed(2)}V`,
      )}</text>`,
    );
  }

  const timeTicks = 5;
  for (let i = 0; i <= timeTicks; i += 1) {
    const fraction = i / timeTicks;
    const x = pad.left + (width - pad.left - pad.right) * fraction;
    rows.push(`<line x1="${x}" x2="${x}" y1="${pad.top}" y2="${height - pad.bottom}" class="telemetry-grid"></line>`);
    const value = timeRange.max * fraction;
    rows.push(
      `<text x="${x}" y="${height - 18}" text-anchor="middle" class="telemetry-label">${escapeHtml(formatElapsedShort(value))}</text>`,
    );
  }

  rows.push(
    `<line x1="${pad.left}" x2="${width - pad.right}" y1="${height - pad.bottom}" y2="${height - pad.bottom}" class="telemetry-axis"></line>`,
    `<line x1="${pad.left}" x2="${pad.left}" y1="${pad.top}" y2="${height - pad.bottom}" class="telemetry-axis"></line>`,
  );

  sessions.forEach((entry, index) => {
    const color = SERIES_COLORS[index % SERIES_COLORS.length];
    const data = entry.samples;
    const points = data.map((sample) => {
      const x = mapValue(sample.elapsedSeconds, timeRange.min, timeRange.max, pad.left, width - pad.right);
      const value = mode === "percent" ? sample.percent ?? estimatePercentFromVoltage(sample.voltage) : sample.voltage;
      const y = mapValue(value, valueRange.min, valueRange.max, height - pad.bottom, pad.top);
      return { x, y, value };
    });
    const path = points
      .map((point, pointIndex) => `${pointIndex === 0 ? "M" : "L"} ${point.x.toFixed(1)} ${point.y.toFixed(1)}`)
      .join(" ");
    rows.push(`<path d="${path}" class="telemetry-series" style="stroke:${color}"></path>`);
    const lastPoint = points[points.length - 1];
    rows.push(
      `<circle cx="${lastPoint.x.toFixed(1)}" cy="${lastPoint.y.toFixed(1)}" r="4.5" class="telemetry-dot" style="fill:${color}"></circle>`,
    );
    legend.push(makeLegendItem(entry.detail.label || entry.detail.id, color, mode === "percent" ? "percent" : "voltage"));
  });

  rows.push(
    `<text x="${pad.left}" y="16" class="telemetry-label">${escapeHtml(mode === "percent" ? "Percent mode" : "Voltage mode")}</text>`,
    `<text x="${width - pad.right}" y="16" text-anchor="end" class="telemetry-label">${escapeHtml(
      `range ${formatRange(valueRange, mode)}`,
    )}</text>`,
  );

  return { svg: rows.join(""), legend };
}

function makeLegendItem(label, color, unit) {
  const item = document.createElement("div");
  item.className = "legend__item";
  const swatch = document.createElement("span");
  swatch.className = "legend__swatch";
  swatch.style.background = color;
  const text = document.createElement("span");
  text.textContent = `${label} · ${unit}`;
  item.append(swatch, text);
  return item;
}

function deriveTimeRange(sessions) {
  let max = 0;
  sessions.forEach((entry) => {
    const sessionMax = entry.samples.at(-1)?.elapsedSeconds ?? 0;
    max = Math.max(max, sessionMax);
  });
  return { min: 0, max: max || 1 };
}

function deriveVoltageRange(sessions) {
  const values = sessions.flatMap((entry) => entry.samples.map((sample) => sample.voltage)).filter((value) => Number.isFinite(value));
  const minValue = values.length ? Math.min(...values) : 3.2;
  const maxValue = values.length ? Math.max(...values) : 4.2;
  const padding = Math.max(0.05, (maxValue - minValue) * 0.18);
  return {
    min: clamp(minValue - padding, 3.0, 4.35),
    max: clamp(maxValue + padding, 3.25, 4.35),
  };
}

function formatRange(range, mode) {
  if (mode === "percent") {
    return `${Math.round(range.min)}-${Math.round(range.max)}%`;
  }
  return `${range.min.toFixed(2)}-${range.max.toFixed(2)}V`;
}

function metric(label, value) {
  const wrap = document.createElement("div");
  wrap.className = "metric";
  const title = document.createElement("div");
  title.className = "metric__label";
  title.textContent = label;
  const val = document.createElement("div");
  val.className = "metric__value";
  val.innerHTML = escapeHtml(value);
  wrap.append(title, val);
  return wrap;
}

function badgeFor(text, tone) {
  const el = document.createElement("span");
  const classes = ["badge"];
  if (tone === "good") {
    classes.push("badge--good");
  } else if (tone === "warn") {
    classes.push("badge--warn");
  } else if (tone === "danger") {
    classes.push("badge--danger");
  }
  el.className = classes.join(" ");
  el.textContent = text;
  return el;
}

function setStatus(heading, message) {
  dom.connectionState.textContent = heading;
  dom.refreshState.textContent = message;
  dom.connectionState.classList.add("status-chip--live");
  dom.connectionState.classList.remove("status-chip--warn");
}

function setFixtureIndicator(isFixture) {
  dom.fixtureState.hidden = !isFixture;
  if (isFixture) {
    dom.fixtureState.textContent = "Fixture mode";
  }
}

function applyMode(mode) {
  state.selectedMode = mode === "percent" ? "percent" : "voltage";
  dom.modeButtons.forEach((button) => {
    const active = button.dataset.mode === state.selectedMode;
    button.classList.toggle("is-active", active);
    button.setAttribute("aria-pressed", active ? "true" : "false");
  });
}

function readSavedMode() {
  try {
    const saved = window.localStorage.getItem(MODE_STORAGE_KEY);
    return saved === "percent" ? "percent" : "voltage";
  } catch {
    return "voltage";
  }
}

function saveMode(mode) {
  try {
    window.localStorage.setItem(MODE_STORAGE_KEY, mode);
  } catch {
    // Storage can be unavailable in kiosk contexts.
  }
}

function parseDate(value) {
  if (!value) {
    return null;
  }
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? null : date;
}

function formatClock(date) {
  return new Intl.DateTimeFormat(undefined, {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).format(date);
}

function formatTimestamp(date) {
  if (!date) {
    return "n/a";
  }
  return new Intl.DateTimeFormat(undefined, {
    month: "short",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  }).format(date);
}

function formatDuration(durationSeconds, startedAt, endedAt) {
  let total = durationSeconds;
  if (total === null) {
    const start = startedAt?.getTime?.() || 0;
    const end = endedAt?.getTime?.() || 0;
    total = start && end ? Math.max(0, Math.round((end - start) / 1000)) : 0;
  }
  const minutes = Math.floor(total / 60);
  const seconds = total % 60;
  return `${minutes}m ${String(seconds).padStart(2, "0")}s`;
}

function formatElapsedShort(seconds) {
  if (!Number.isFinite(seconds)) {
    return "0s";
  }
  if (seconds < 60) {
    return `${Math.round(seconds)}s`;
  }
  const minutes = Math.floor(seconds / 60);
  const remainder = Math.round(seconds % 60);
  return `${minutes}m ${String(remainder).padStart(2, "0")}s`;
}

function formatAge(lastSeen, ageSeconds) {
  if (ageSeconds !== null) {
    if (ageSeconds < 60) {
      return `${ageSeconds}s ago`;
    }
    const minutes = Math.floor(ageSeconds / 60);
    const seconds = ageSeconds % 60;
    return `${minutes}m ${String(seconds).padStart(2, "0")}s ago`;
  }
  if (!lastSeen) {
    return "unknown";
  }
  return formatTimestamp(lastSeen);
}

function formatVoltage(value, compact = false) {
  if (!Number.isFinite(value)) {
    return "n/a";
  }
  return compact ? `${value.toFixed(2)}V` : `${value.toFixed(2)} V`;
}

function formatPercent(value, compact = false) {
  if (!Number.isFinite(value)) {
    return "n/a";
  }
  return compact ? `${Math.round(value)}%` : `${Math.round(value)} %`;
}

function estimatePercentFromVoltage(voltage) {
  if (!Number.isFinite(voltage)) {
    return 0;
  }
  const curve = [
    [3.2, 0],
    [3.45, 5],
    [3.62, 10],
    [3.73, 20],
    [3.82, 35],
    [3.92, 50],
    [4.0, 68],
    [4.08, 82],
    [4.15, 92],
    [4.2, 100],
  ];
  if (voltage <= curve[0][0]) {
    return curve[0][1];
  }
  for (let index = 1; index < curve.length; index += 1) {
    const [x1, y1] = curve[index - 1];
    const [x2, y2] = curve[index];
    if (voltage <= x2) {
      const t = (voltage - x1) / (x2 - x1);
      return y1 + t * (y2 - y1);
    }
  }
  return 100;
}

function mapValue(value, min, max, outMin, outMax) {
  if (!Number.isFinite(value)) {
    return outMin;
  }
  if (max === min) {
    return (outMin + outMax) / 2;
  }
  const ratio = (value - min) / (max - min);
  return outMin + clamp(ratio, 0, 1) * (outMax - outMin);
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function toNumber(value) {
  if (value === null || value === undefined || value === "") {
    return null;
  }
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function toInteger(value) {
  const number = toNumber(value);
  return number === null ? null : Math.trunc(number);
}

function toBoolean(value) {
  if (typeof value === "boolean") {
    return value;
  }
  if (typeof value === "number") {
    return value !== 0;
  }
  if (typeof value === "string") {
    return ["1", "true", "yes", "on", "online", "connected", "charging", "usb"].includes(value.trim().toLowerCase());
  }
  return Boolean(value);
}

function pick(object, keys) {
  if (!object) {
    return undefined;
  }
  for (const key of keys) {
    if (Object.prototype.hasOwnProperty.call(object, key) && object[key] !== undefined && object[key] !== null && object[key] !== "") {
      return object[key];
    }
  }
  return undefined;
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function deepClone(value) {
  return JSON.parse(JSON.stringify(value));
}

function buildCsvDataUrl(sessionId) {
  const session = state.sessionCache.get(sessionId)?.detail || normalizeSession(state.fixture.sessionDetails[sessionId] || {}, sessionId);
  const samples = state.sessionCache.get(sessionId)?.samples || normalizeSamples({ samples: state.fixture.samples[sessionId] || [] }, session);
  const rows = ["elapsed_s,voltage_v,percent"];
  samples.forEach((sample) => {
    rows.push([sample.elapsedSeconds, sample.voltage, sample.percent ?? ""].join(","));
  });
  return `data:text/csv;charset=utf-8,${encodeURIComponent(`${rows.join("\n")}\n`)}`;
}
