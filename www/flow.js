"use strict";

const pollInterval = 200;
const relaySpacing = 1000 / 60;
const historyLimit = 64;
const dots = document.getElementById("flow-dots");
const relayScale = document.getElementById("flow-relay-scale");
const messageList = document.getElementById("flow-messages");
const chatFrame = document.querySelector(".flow-chat");
const fields = [...document.querySelectorAll("[data-stat]")];
const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)");
const flow = new BroadcastChannel("flow");
const rows = [];
let generation = 0;
let nextRelayAt = 0;

function endpoint(name, url) {
  return {
    url,
    station: document.getElementById(`flow-${name}`),
    status: document.getElementById(`flow-${name}-status`),
    fields: fields.filter(field => field.dataset.stat.startsWith(`${name}.`)),
    busy: false,
    snapshot: null,
    generation: 0
  };
}

const server = endpoint("server", "/_stat");
const backend = endpoint("backend", "/ws/stat");

function animateDot(type, points, duration, delay = 0) {
  if (reducedMotion.matches) return;
  const dot = document.createElement("span");
  dot.className = `flow-dot flow-${type}`;
  dots.append(dot);
  const animation = dot.animate(points, { duration, delay, easing: "linear" });
  animation.finished.then(() => dot.remove(), () => dot.remove());
}

function showRelays(previous, current) {
  if (!current) {
    relayScale.textContent = "중계 횟수 조회 불가";
    return;
  }
  const request = Math.max(0, current.up.n - (previous?.up.n ?? current.up.n));
  const response = Math.max(0, current.down.n - (previous?.down.n ?? current.down.n));
  const total = request + response;
  const now = performance.now();
  const start = Math.max(now, nextRelayAt);
  const slots = Math.ceil((now + pollInterval - start) / relaySpacing);
  const scale = Math.max(1, Math.ceil(total / Math.max(1, slots)));
  relayScale.textContent = `회색 점 1개 ≈ 중계 ${scale.toLocaleString("ko-KR")}회`;
  if (!total || slots <= 0 || reducedMotion.matches) return;

  const count = Math.min(slots, Math.ceil(total / scale));
  const up = Math.round(count * request / total);
  for (let i = 0; i < count; i++) {
    const requestLane = Math.round((i + 1) * up / count) > Math.round(i * up / count);
    const top = requestLane ? "25%" : "75%";
    const ends = requestLane ? ["16.666%", "83.333%"] : ["83.333%", "16.666%"];
    animateDot("relay", ends.map(left => ({ left, top, opacity: 1 })), 650,
      start - now + i * relaySpacing);
  }
  nextRelayAt = start + count * relaySpacing;
}

function showMetrics(target, snapshot) {
  const label = snapshot ? "응답 중" : "응답 없음";
  if (target.status.textContent !== label) target.status.textContent = label;
  target.station.dataset.state = snapshot ? "online" : "offline";
  for (const field of target.fields) {
    const value = field.dataset.stat.split(".").slice(1)
      .reduce((parent, key) => parent?.[key], snapshot);
    field.textContent = Number.isFinite(value) ? value.toLocaleString("ko-KR") : "-";
  }
}

function milliseconds(value) {
  return Number.isFinite(value) ? `${value.toFixed(3)} ms` : "-";
}

function showMessage(row, proxyEvents, chatEvents, ack, previousAck) {
  if (!ack) return;
  const recv = chatEvents.findLast(event =>
    event.seq < ack.seq && event.seq > previousAck &&
    event.stage === "recv" && event.kind === "m");
  const toBackend = recv && proxyEvents.findLast(event =>
    event.stage === "S>B" && event.t_us <= recv.t_us);
  const fromBackend = proxyEvents.find(event =>
    event.stage === "B>S" && event.t_us >= ack.t_us);
  row.element.children[1].textContent = milliseconds(toBackend && (recv.t_us - toBackend.t_us) / 1000);
  row.element.children[2].textContent = milliseconds(fromBackend && (fromBackend.t_us - ack.t_us) / 1000);

  const fromClient = toBackend && proxyEvents.findLast(event =>
    event.stage === "A>S" && event.t_us <= toBackend.t_us);
  const toClient = fromBackend && proxyEvents.find(event =>
    event.stage === "S>A" && event.t_us >= fromBackend.t_us);
  const points = [
    [fromClient, "16.666%", "25%"],
    [toBackend, "50%", "25%"],
    [recv, "83.333%", "25%"],
    [ack, "83.333%", "75%"],
    [fromBackend, "50%", "75%"],
    [toClient, "16.666%", "75%"]
  ].filter(([event]) => event)
    .sort(([first], [second]) => first.t_us - second.t_us)
    .map(([, left, top]) => ({ left, top, opacity: 1 }));
  if (points.length > 1) animateDot("trace", points, 240 * (points.length - 1));
}

function connectionStart(events, first) {
  if (first?.stage !== "send" || first.kind !== "w") return 0;
  const index = first.seq - events[0].seq;
  const previous = events[index - 1];
  // A later w follows this trace's recv n, with at most the peer's send l in between.
  if (index >= 2 || (previous && !(previous.stage === "send" && previous.kind === "l"))) {
    return first.seq;
  }
  return 0;
}

function showTraces() {
  if (!server.snapshot || !backend.snapshot) return;
  const sampledThrough = Math.min(server.generation, backend.generation);
  const traces = new Set(rows.filter(row => !row.done).map(row => row.trace));
  for (const trace of traces) {
    const ready = rows.filter(row => row.trace === trace && !row.done &&
      row.generation <= sampledThrough);
    if (!ready.length) continue;
    const lastAck = rows.findLast(row => row.trace === trace && row.ack);
    const events = backend.snapshot.events;
    const chatEvents = events.filter(event => event.trace === trace);
    const baseSeq = lastAck?.ack ?? connectionStart(events, chatEvents[0]);
    const baseNumber = lastAck?.n ?? 0;
    // Without the events after the last known ordinal, a missing ACK shifts every index.
    const contiguous = events[0]?.seq <= baseSeq + 1;
    const acks = contiguous ? chatEvents.filter(event =>
      event.seq > baseSeq && event.stage === "send" && event.kind === "a") : [];
    const proxyEvents = server.snapshot.events.filter(event => event.trace === trace)
      .sort((a, b) => a.t_us - b.t_us || a.seq - b.seq);
    for (const row of ready) {
      const index = row.n - baseNumber - 1;
      const ack = acks[index];
      const previousAck = acks[index - 1]?.seq ?? baseSeq;
      showMessage(row, proxyEvents, chatEvents, ack, previousAck);
      row.done = true;
      if (ack) row.ack = ack.seq;
    }
  }
}

async function poll(target) {
  if (target.busy) return;
  target.busy = true;
  const startedThrough = generation;
  let snapshot = null;
  try {
    const response = await fetch(target.url, { cache: "no-store", signal: AbortSignal.timeout(2000) });
    if (response.ok) {
      const data = await response.json();
      if (Array.isArray(data?.events) &&
          (target !== server || (Number.isFinite(data.up?.n) && Number.isFinite(data.down?.n)))) {
        snapshot = data;
      }
    }
  } catch {
    // A failed or timed-out request has no current values.
  } finally {
    target.busy = false;
  }
  if (target === server) showRelays(target.snapshot, snapshot);
  target.snapshot = snapshot;
  target.generation = startedThrough;
  showMetrics(target, snapshot);
  showTraces();
}

function refresh() {
  poll(server);
  poll(backend);
}

function addMessage(data) {
  if (!Number.isSafeInteger(data?.trace) || data.trace <= 0 ||
      !Number.isSafeInteger(data.n) || data.n <= 0 ||
      !Number.isFinite(data.rtt_ms) || data.rtt_ms < 0 ||
      rows.some(row => row.trace === data.trace && row.n === data.n)) return;

  const element = document.createElement("tr");
  for (const value of [data.n, "-", "-", milliseconds(data.rtt_ms)]) {
    const cell = document.createElement("td");
    cell.textContent = value;
    element.append(cell);
  }
  messageList.append(element);
  rows.push({ trace: data.trace, n: data.n, generation: ++generation, element });
  if (rows.length > historyLimit) rows.shift().element.remove();
}

flow.addEventListener("message", ({ data }) => addMessage(data));
window.addEventListener("message", ({ source, origin, data }) => {
  if (source === chatFrame.contentWindow && origin === location.origin) addMessage(data);
});

refresh();
setInterval(refresh, pollInterval);
