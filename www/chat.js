"use strict";

const status = document.getElementById("status");
const counts = document.getElementById("counts");
const start = document.getElementById("start");
const cancel = document.getElementById("cancel");
const next = document.getElementById("next");
const leave = document.getElementById("leave");
const reconnect = document.getElementById("reconnect");
const messages = document.getElementById("messages");
const composer = document.getElementById("composer");
const message = document.getElementById("message");
const imageButton = document.getElementById("image-button");
const imageFile = document.getElementById("image-file");
const notice = document.getElementById("notice");

const maxText = 1000;
const maxImage = 2 * 1024 * 1024;
const chunkSize = 16 * 1024;
const bufferLimit = 256 * 1024;
const imageTypes = { jpg: "image/jpeg", png: "image/png" };
const flow = new BroadcastChannel("flow");
const stateLabels = {
  start: "채팅을 시작할 수 있습니다.",
  connecting: "서버에 연결하는 중입니다.",
  waiting: "상대를 찾는 중입니다.",
  chatting: "상대와 연결되었습니다.",
  departed: "상대가 나갔습니다.",
  disconnected: "연결이 끊겼습니다.",
};

let state = "start";
let socket;
let trace;
let sentCount = 0;
let pendingAcks = [];
let incomingImage;
let upload;
let composing = false;
const imageUrls = [];

function showState(value) {
  const focused = document.activeElement;
  state = value;
  status.textContent = stateLabels[value];
  start.hidden = value !== "start";
  cancel.hidden = value !== "connecting" && value !== "waiting";
  next.hidden = value !== "chatting" && value !== "departed";
  leave.hidden = value !== "chatting";
  reconnect.hidden = value !== "disconnected";
  counts.hidden = value !== "waiting";
  messages.hidden = value !== "chatting" && value !== "departed";
  composer.hidden = value !== "chatting";

  if (focused?.closest("[hidden]")) {
    const focusTargets = {
      start, connecting: cancel, waiting: cancel, chatting: message,
      departed: next, disconnected: reconnect,
    };
    focusTargets[value].focus();
  }
}

function stopConversation() {
  pendingAcks = [];
  incomingImage = undefined;
  upload = undefined;
  imageButton.disabled = false;
  notice.textContent = "";
}

function clearMessages() {
  messages.replaceChildren();
  for (const url of imageUrls) URL.revokeObjectURL(url);
  imageUrls.length = 0;
  message.value = "";
  composing = false;
}

function appendMessage(mine, content) {
  const row = document.createElement("li");
  row.className = mine ? "chat-message chat-message-own" : "chat-message";
  const sender = document.createElement("span");
  sender.className = "chat-sender";
  sender.textContent = mine ? "나" : "상대";
  row.append(sender, content);
  messages.append(row);
  messages.scrollTop = messages.scrollHeight;
}

function appendText(mine, text) {
  const content = document.createElement("p");
  content.textContent = text;
  appendMessage(mine, content);
}

function appendImage(mine, blob) {
  const url = URL.createObjectURL(blob);
  imageUrls.push(url);
  const image = document.createElement("img");
  image.alt = mine ? "내가 보낸 이미지" : "상대가 보낸 이미지";
  image.src = url;
  appendMessage(mine, image);
}

function receiveImageChunk(chunk) {
  if (state !== "chatting" || !incomingImage || chunk.byteLength === 0) return;
  const image = incomingImage;
  image.remaining -= chunk.byteLength;
  if (image.remaining < 0) {
    incomingImage = undefined;
    return;
  }
  image.chunks.push(chunk);
  if (image.remaining === 0) {
    appendImage(false, new Blob(image.chunks, { type: image.type }));
    incomingImage = undefined;
  }
}

function receive(data) {
  if (typeof data !== "string") {
    receiveImageChunk(data);
  } else if (data === "w") {
    showState("waiting");
  } else if (data === "c") {
    stopConversation();
    clearMessages();
    showState("chatting");
  } else if (data === "l") {
    stopConversation();
    showState("departed");
  } else if (state === "waiting" && data.startsWith("s")) {
    const stats = /^s([0-9]+) ([0-9]+)$/.exec(data);
    if (stats) counts.textContent = `접속 ${stats[1]}명 · 대기 ${stats[2]}명`;
  } else if (state === "chatting") {
    if (data.startsWith("m")) {
      appendText(false, data.slice(1));
    } else if (data.startsWith("i")) {
      const header = /^i(jpg|png) ([0-9]{1,7})$/.exec(data);
      if (!header || incomingImage) return;
      const size = Number(header[2]);
      if (size < 1 || size > maxImage) return;
      incomingImage = { type: imageTypes[header[1]], remaining: size, chunks: [] };
    } else if (data === "a" && pendingAcks.length > 0) {
      const sent = pendingAcks.shift();
      flow.postMessage({ trace, n: sent.n, rtt_ms: performance.now() - sent.time });
    }
  }
}

function connect() {
  stopConversation();
  clearMessages();
  sentCount = 0;
  trace = (crypto.getRandomValues(new Uint32Array(1))[0] & 0x7fffffff) || 1;
  counts.textContent = "접속 및 대기 수를 확인하는 중입니다.";
  showState("connecting");
  const current = new WebSocket(`ws://${location.host}/ws/chat?trace=${trace}`);
  socket = current;
  current.binaryType = "arraybuffer";
  current.addEventListener("message", (event) => {
    if (socket === current) receive(event.data);
  });
  current.addEventListener("close", () => {
    if (socket !== current) return;
    socket = undefined;
    stopConversation();
    clearMessages();
    showState("disconnected");
  });
}

function goToStart() {
  const current = socket;
  socket = undefined;
  current?.close();
  stopConversation();
  clearMessages();
  showState("start");
}

start.addEventListener("click", connect);
reconnect.addEventListener("click", connect);
cancel.addEventListener("click", goToStart);
leave.addEventListener("click", goToStart);
next.addEventListener("click", () => {
  if (socket?.readyState !== WebSocket.OPEN) return;
  stopConversation();
  clearMessages();
  counts.textContent = "접속 및 대기 수를 확인하는 중입니다.";
  socket.send("n");
  showState("waiting");
});

message.addEventListener("compositionstart", () => { composing = true; });
message.addEventListener("compositionend", () => { composing = false; });
message.addEventListener("keydown", (event) => {
  if (event.key !== "Enter" || event.isComposing || composing) return;
  event.preventDefault();
  composer.requestSubmit();
});
composer.addEventListener("submit", (event) => {
  event.preventDefault();
  if (state !== "chatting" || socket?.readyState !== WebSocket.OPEN || composing) return;
  const text = message.value;
  if (!text.trim()) return;
  if ([...text].length > maxText) {
    notice.textContent = "메시지는 1,000자까지 보낼 수 있습니다.";
    return;
  }
  pendingAcks.push({ n: ++sentCount, time: performance.now() });
  socket.send(`m${text}`);
  appendText(true, text);
  message.value = "";
  notice.textContent = "";
});

imageButton.addEventListener("click", () => imageFile.click());
imageFile.addEventListener("change", async () => {
  const file = imageFile.files[0];
  imageFile.value = "";
  if (!file || state !== "chatting" || socket?.readyState !== WebSocket.OPEN || upload) return;
  if (file.size < 1 || file.size > maxImage) {
    notice.textContent = "이미지는 2MB까지 보낼 수 있습니다.";
    return;
  }

  const current = socket;
  const job = {};
  const active = () => upload === job && current.readyState === WebSocket.OPEN;
  upload = job;
  imageButton.disabled = true;
  notice.textContent = "";
  try {
    const bytes = new Uint8Array(await file.slice(0, 4).arrayBuffer());
    if (!active()) return;
    let kind;
    if (bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff) kind = "jpg";
    if (bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47) kind = "png";
    if (!kind) {
      notice.textContent = "JPG 또는 PNG를 선택해 주세요.";
      return;
    }

    current.send(`i${kind} ${file.size}`);
    for (let offset = 0; offset < file.size; offset += chunkSize) {
      while (active() && current.bufferedAmount > bufferLimit) {
        await new Promise((resolve) => setTimeout(resolve, 20));
      }
      if (!active()) return;
      current.send(file.slice(offset, offset + chunkSize));
    }
    appendImage(true, file.slice(0, file.size, imageTypes[kind]));
  } catch {
    if (active()) notice.textContent = "이미지를 보낼 수 없습니다.";
  } finally {
    if (upload === job) {
      upload = undefined;
      imageButton.disabled = false;
    }
  }
});
