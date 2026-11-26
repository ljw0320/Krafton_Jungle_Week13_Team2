'use strict';

const chatStatus = document.getElementById('chat-status');
const chatNotice = document.getElementById('chat-notice');
const messages = document.getElementById('messages');
const composer = document.getElementById('composer');
const message = document.getElementById('message');
const send = document.getElementById('send');
const reconnect = document.getElementById('reconnect');
const encoder = new TextEncoder();
const messageLimit = 4096;
let socket;

function appendChatMessage(text, source) {
  const atBottom = messages.scrollHeight - messages.scrollTop - messages.clientHeight < 8;
  const item = document.createElement('li');
  item.className = 'chat-message';
  if (source) {
    const label = document.createElement('small');
    label.className = 'live-bot-source';
    label.textContent = source;
    const content = document.createElement('span');
    content.textContent = text;
    item.append(label, content);
  } else {
    item.textContent = text;
  }
  messages.append(item);
  if (messages.children.length > 200) messages.firstElementChild.remove();
  if (atBottom) messages.scrollTop = messages.scrollHeight;
}

function connectChat() {
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  chatStatus.textContent = '공용방에 연결하는 중입니다.';
  chatNotice.textContent = '';
  reconnect.hidden = true;
  send.disabled = true;
  socket = new WebSocket(`${scheme}//${location.host}/ws/live`);
  let backend;

  socket.addEventListener('message', event => {
    if (!backend) {
      try {
        backend = JSON.parse(event.data).backend;
      } catch {
        backend = null;
      }
      if (backend !== 'A' && backend !== 'B') {
        backend = null;
        chatNotice.textContent = '백엔드 연결 정보를 확인할 수 없습니다.';
        socket.close();
        return;
      }
      chatStatus.textContent = `연결됨 · 백엔드 ${backend}`;
      send.disabled = false;
      return;
    }

    appendChatMessage(event.data);
  });

  socket.addEventListener('close', () => {
    chatStatus.textContent = '공용방 연결이 끊겼습니다.';
    send.disabled = true;
    reconnect.hidden = false;
  });
}

reconnect.addEventListener('click', connectChat);
composer.addEventListener('submit', event => {
  event.preventDefault();
  if (send.disabled || socket.readyState !== WebSocket.OPEN) return;
  const text = message.value;
  if (!text.trim()) return;
  if (encoder.encode(text).length > messageLimit) {
    chatNotice.textContent = '메시지를 4,096바이트 이하로 줄여 주세요.';
    return;
  }
  socket.send(text);
  message.value = '';
  chatNotice.textContent = '';
  message.focus();
});
message.addEventListener('keydown', event => {
  if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
    event.preventDefault();
    if (!send.disabled) composer.requestSubmit();
  }
});
connectChat();

const runTest = document.getElementById('run-test');
const testStatus = document.getElementById('test-status');
const testApi = '/api/live-test';
let monitoringTest = false;
let requesting = false;
let startRequested = false;
let testPollTimer;
let feedRunId = null;
let feedCount = 0;
const renderedResults = [];

function renderTestFeed(runId, feed) {
  if (typeof runId !== 'string' || !runId || !Array.isArray(feed)) return;
  if (runId !== feedRunId) {
    feedRunId = runId;
    feedCount = 0;
  }
  for (; feedCount < feed.length; feedCount++) {
    const {round, from, text} = feed[feedCount];
    appendChatMessage(text, `${round}회차 · 봇 ${from}`);
  }
}

function addMetric(list, label, value, detail) {
  const row = document.createElement('div');
  const name = document.createElement('dt');
  const measure = document.createElement('dd');
  name.textContent = label;
  measure.textContent = value;
  if (detail) {
    const small = document.createElement('small');
    small.textContent = detail;
    measure.append(small);
  }
  row.append(name, measure);
  list.append(row);
}

function renderResult(round, result) {
  const card = document.getElementById(`round-${round}`);
  card.replaceChildren();
  if (!result) return;

  const counts = document.createElement('dl');
  for (const backend of ['A', 'B']) {
    const bots = result.assignments[backend];
    addMetric(counts, `백엔드 ${backend} 연결`, `${bots.length}명`, bots.join(', '));
  }
  addMetric(counts, '기대 수신', `${result.expected}건`);
  addMetric(counts, '실제 수신', `${result.received}건`);
  addMetric(counts, '누락', `${result.missing}건`);
  addMetric(counts, '중복', `${result.duplicates}건`);
  addMetric(counts, '순서 불일치', `${result.out_of_order}건`);
  for (const [name, latency] of [['p50', result.p50_ms], ['p95', result.p95_ms]]) {
    addMetric(counts, `전달 지연 ${name}`, latency === null ? '—' : `${latency.toFixed(2)} ms`);
  }
  if (round === 2) {
    addMetric(counts, 'A → B 수신', `${result.cross_backend.A_to_B}건`);
    addMetric(counts, 'B → A 수신', `${result.cross_backend.B_to_A}건`);
  }
  card.append(counts);

  if (!result.recent.length) return;
  const recent = document.createElement('ul');
  recent.className = 'live-recent';
  const heading = document.createElement('h4');
  heading.textContent = '최근 메시지 수신 기록';
  for (const {sequence, text, from, backend, received_by} of result.recent) {
    const item = document.createElement('li');
    const source = document.createElement('small');
    const content = document.createElement('p');
    const receivers = document.createElement('small');
    source.textContent = `#${sequence} · ${from} · 백엔드 ${backend}`;
    content.textContent = text;
    receivers.textContent = `수신 · ${received_by.join(', ') || '없음'}`;
    item.append(source, content, receivers);
    recent.append(item);
  }
  card.append(heading, recent);
}

function renderStatus(data) {
  const round = data.round === 1 || data.round === 2 ? `백엔드 ${data.round}개` : '';
  const labels = {
    idle: '대기',
    running: round ? `실행 중 · ${round}` : '실행 중 · 준비 중',
    completed: '완료 · 두 회차 모두 성공',
    failed: ['실패', round, data.error].filter(Boolean).join(' · ')
  };
  if (!Object.hasOwn(labels, data.status)) throw new Error('알 수 없는 시험 상태');
  for (const round of [1, 2]) {
    const result = data.results[round];
    const snapshot = JSON.stringify(result);
    if (snapshot === renderedResults[round]) continue;
    renderResult(round, result);
    renderedResults[round] = snapshot;
  }
  renderTestFeed(data.run_id, data.messages);
  testStatus.dataset.state = data.status;
  if (testStatus.textContent !== labels[data.status]) {
    testStatus.textContent = labels[data.status];
  }
  monitoringTest = data.status === 'running';
}

async function requestTest(method) {
  if (requesting || (method === 'POST' && monitoringTest)) return;
  clearTimeout(testPollTimer);
  requesting = true;
  if (method === 'POST') {
    // The trial may start even if the response never reaches this page.
    monitoringTest = true;
    runTest.disabled = true;
    testStatus.textContent = '시험 시작을 요청하는 중입니다.';
    testStatus.dataset.state = 'running';
  }
  try {
    const response = await fetch(testApi, {
      method,
      cache: 'no-store',
      signal: AbortSignal.timeout(5000)
    });
    if (!response.ok && !(method === 'POST' && response.status === 409)) {
      if (method === 'POST' && response.status >= 400 && response.status < 500) {
        monitoringTest = false;
      }
      throw new Error(`HTTP ${response.status}`);
    }
    renderStatus(await response.json());
  } catch {
    testStatus.dataset.state = monitoringTest ? 'running' : 'failed';
    testStatus.textContent = monitoringTest
      ? '시험 상태를 확인하지 못했습니다. 다시 확인하는 중입니다.'
      : '시험 서버에 연결할 수 없습니다. 다시 시도해 주세요.';
  } finally {
    requesting = false;
    runTest.disabled = monitoringTest;
    if (startRequested && !monitoringTest) {
      startRequested = false;
      void requestTest('POST');
    } else {
      startRequested = false;
      testPollTimer = setTimeout(() => requestTest('GET'), monitoringTest ? 350 : 1500);
    }
  }
}

runTest.addEventListener('click', () => {
  if (monitoringTest || startRequested) return;
  if (requesting) {
    startRequested = true;
    runTest.disabled = true;
    return;
  }
  void requestTest('POST');
});
requestTest('GET');
