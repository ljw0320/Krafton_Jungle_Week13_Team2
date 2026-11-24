(() => {
  const form = document.getElementById("cgi-form");
  const method = document.getElementById("cgi-method");
  const inputA = document.getElementById("cgi-a");
  const inputB = document.getElementById("cgi-b");
  const invalid = document.getElementById("cgi-invalid");
  const missing = document.getElementById("cgi-missing");
  const summary = document.getElementById("cgi-summary");
  const requestView = document.getElementById("cgi-request");
  const expectedView = document.getElementById("cgi-expected");
  const responseView = document.getElementById("cgi-response");
  const bodyView = document.getElementById("cgi-body");
  const controls = [...form.elements, invalid, missing];
  let running = false;

  function setResult(message, result = "") {
    summary.textContent = message;
    summary.dataset.result = result;
  }

  // BigInt를 사용해 C CGI가 처리하는 64비트 정수를 반올림 없이 비교합니다.
  function additionConfig() {
    const a = BigInt(inputA.value);
    const b = BigInt(inputB.value);
    const sum = a + b;
    const min = -(2n ** 63n), max = 2n ** 63n - 1n;
    if ([a, b, sum].some(value => value < min || value > max))
      throw new Error("a, b와 합계는 부호 있는 64비트 정수 범위여야 합니다.");
    const body = `{"a":${a},"b":${b},"sum":${sum}}`;
    return { params: new URLSearchParams({ a: String(a), b: String(b) }),
      status: 200, expected: body, matches: text => text.trim() === body };
  }

  // 실제 본문 수신까지 기다린 후 상태·타입·내용을 예상 결과와 비교합니다.
  async function run(config) {
    if (running || location.protocol === "file:") return;
    running = true;
    controls.forEach(control => { control.disabled = true; });
    const verb = method.value;
    const path = config.path || "/cgi-bin/adder";
    const encoded = config.params.toString();
    const url = verb === "GET" && encoded ? `${path}?${encoded}` : path;
    const options = { method: verb, cache: "no-store" };
    if (verb === "POST") {
      options.headers = { "Content-Type": "application/x-www-form-urlencoded" };
      options.body = encoded;
    }
    requestView.textContent = `${verb} ${url}` + (verb === "POST" ? `\n본문: ${encoded || "(빈 본문)"}` : "");
    expectedView.textContent = `HTTP ${config.status} · ${config.expected}`;
    responseView.textContent = "수신 대기 중…";
    bodyView.textContent = "";
    setResult("CGI 응답을 기다리는 중…");
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 8000);
    const started = performance.now();
    try {
      const response = await fetch(url, { ...options, signal: controller.signal });
      const text = await response.text();
      const type = response.headers.get("Content-Type") || "(없음)";
      responseView.textContent = `HTTP ${response.status} · ${type} · ${(performance.now() - started).toFixed(1)} ms`;
      bodyView.textContent = text || "(빈 본문)";
      const ok = response.status === config.status && config.matches(text, type);
      const hint = response.status === 404 && config.status !== 404 ? " · CGI 실행 인자와 파일을 확인하세요." : "";
      setResult(ok ? "검증 통과 · 예상 응답과 일치합니다." : `검증 실패 · 예상 응답과 다릅니다.${hint}`, ok ? "pass" : "fail");
    } catch (error) {
      responseView.textContent = "응답을 완료하지 못했습니다.";
      setResult(error.name === "AbortError" ? "검증 실패 · 8초 시간 초과" : "검증 실패 · 서버 연결 또는 응답 수신 오류", "fail");
    } finally {
      clearTimeout(timer);
      running = false;
      controls.forEach(control => { control.disabled = false; });
    }
  }

  form.addEventListener("submit", event => {
    event.preventDefault();
    try {
      const config = additionConfig();
      const matches = config.matches;
      config.matches = (text, type) => type.split(";")[0].trim() === "application/json" && matches(text);
      void run(config);
    } catch (error) {
      setResult(error.message, "fail");
    }
  });

  invalid.addEventListener("click", () => void run({
    params: new URLSearchParams({ a: "12" }), status: 400,
    expected: "b 누락에 대한 JSON 오류 응답",
    matches: (text, type) => {
      try { return type.split(";")[0].trim() === "application/json" && typeof JSON.parse(text).error === "string"; }
      catch { return false; }
    }
  }));
  missing.addEventListener("click", () => void run({
    path: "/cgi-bin/missing-program", params: new URLSearchParams(), status: 404,
    expected: "404 Not Found", matches: text => text === "404 Not Found\n"
  }));

  if (location.protocol === "file:") {
    controls.forEach(control => { control.disabled = true; });
    setResult("실행 중인 서버 주소로 접속해 주세요. HTML 파일을 직접 열면 CGI 요청을 보낼 수 없습니다.", "fail");
  }
})();
