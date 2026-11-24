(() => {
  const definitions = [
    ["reuse", "GET·POST·CGI 연결 재사용", "하나의 TCP 소켓에서 세 요청의 응답 수신"],
    ["idle", "유휴 5초 종료", "추가 요청 없이 약 5초 후 EOF"],
    ["limit", "100번째 응답 후 종료", "100번째 close · 101번째 응답 없음"],
    ["http10", "HTTP/1.0 종료", "keep-alive 요청에도 close + EOF"],
    ["explicit_close", "Connection: close", "종료 요청 후 EOF · 후속 요청 미처리"],
    ["pipeline", "합쳐진 요청·분할 요청", "바이너리 본문 보존 · 4개 응답 순서 유지"],
    ["slots", "16개 슬롯·17번째 거절", "16개 유지 · 17번째 거절 · 기존 연결 정상"]
  ];
  const table = document.getElementById("keep-alive-results");
  const summary = document.getElementById("keep-alive-summary");
  const target = document.getElementById("keep-alive-target");
  const evidence = document.getElementById("keep-alive-evidence");
  const reportInput = document.getElementById("keep-alive-report");
  const commandView = document.getElementById("keep-alive-command");
  const httpButton = document.getElementById("keep-alive-http-run");
  const httpSummary = document.getElementById("keep-alive-http-summary");
  const httpTable = document.getElementById("keep-alive-http-results");
  const rows = new Map();

  function command(key) {
    return `make\npython3 src/tests/run_keep_alive_checks.py --case ${key} --output keep-alive-report.json`;
  }

  async function copyCommand(key) {
    const value = command(key);
    commandView.textContent = value;
    try {
      await navigator.clipboard.writeText(value);
      summary.textContent = "명령을 복사했습니다. WSL 프로젝트 폴더에서 실행한 뒤 JSON 보고서를 불러오세요.";
    } catch {
      summary.textContent = "명령을 위에 표시했습니다. 복사하여 WSL 프로젝트 폴더에서 실행하세요.";
    }
    summary.dataset.result = "";
  }

  function resetRows() {
    for (const row of rows.values()) {
      row.cells[2].textContent = "별도 실행기 결과를 불러오세요.";
      row.cells[3].textContent = "—";
      row.cells[4].textContent = "미검증";
      row.className = "";
    }
  }

  for (const [key, label, expected] of definitions) {
    const row = table.insertRow();
    for (const value of [label, expected, "", "—", "미검증"])
      row.insertCell().textContent = value;
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = "명령 복사";
    button.setAttribute("aria-label", `${label} 검사 명령 복사`);
    button.addEventListener("click", () => void copyCommand(key));
    row.insertCell().append(button);
    rows.set(key, row);
  }
  resetRows();

  reportInput.addEventListener("change", async () => {
    const file = reportInput.files[0];
    if (!file) return;
    resetRows();
    evidence.textContent = "보고서를 확인하고 있습니다…";
    target.textContent = "보고서 확인 중…";
    try {
      if (file.size > 262144) throw new Error("보고서는 256KiB 이하여야 합니다.");
      const report = JSON.parse(await file.text());
      const results = report.results;
      if (report.format_version !== 1 || report.scope !== "isolated-executable" ||
          report.target_stopped !== true || !Number.isFinite(Date.parse(report.generated_at)) ||
          !Array.isArray(results) || results.length < 1 || results.length > definitions.length ||
          new Set(results.map(item => item.key)).size !== results.length ||
          results.some(item => !rows.has(item.key) || typeof item.passed !== "boolean" ||
            typeof item.actual !== "string" || !Number.isFinite(item.elapsed_ms) || item.elapsed_ms < 0))
        throw new Error("별도 실행기의 JSON 보고서 형식과 항목을 확인하세요.");
      for (const item of results) {
        const row = rows.get(item.key);
        row.cells[2].textContent = item.actual.slice(0, 1000);
        row.cells[3].textContent = `${item.elapsed_ms.toFixed(1)} ms`;
        row.cells[4].textContent = item.passed ? "통과" : "실패";
        row.className = item.passed ? "request-success" : "request-failure";
      }
      const passed = results.filter(item => item.passed).length;
      summary.textContent = `보고서: 통과 ${passed} / 실패 ${results.length - passed} · 미포함 ${definitions.length - results.length}개`;
      summary.dataset.result = passed === results.length ? "pass" : "fail";
      target.textContent = `${file.name} · 실행 시각 ${new Date(report.generated_at).toLocaleString()} · 임시 서버 종료 확인`;
      evidence.textContent = JSON.stringify(report, null, 2);
    } catch (error) {
      summary.textContent = `보고서를 불러오지 못했습니다. ${error.message}`;
      summary.dataset.result = "fail";
      target.textContent = "유효한 보고서가 선택되지 않았습니다.";
      evidence.textContent = "별도 실행기에서 만든 JSON 파일을 선택하세요.";
    }
    reportInput.value = "";
  });

  document.getElementById("keep-alive-copy-all").addEventListener("click", () => void copyCommand("all"));

  const probes = [
    { name: "GET /", url: "/", options: { method: "GET" },
      matches: (text, type) => type.startsWith("text/html") && text.length > 0 },
    { name: "POST /echo", url: "/echo", options: { method: "POST", body: "browser-echo",
      headers: { "Content-Type": "application/octet-stream" } },
      matches: (text, type) => type.startsWith("application/octet-stream") && text === "browser-echo" },
    { name: "GET /cgi-bin/adder", url: "/cgi-bin/adder?a=12&b=30", options: { method: "GET" }, matches: addition },
    { name: "POST /cgi-bin/adder", url: "/cgi-bin/adder", options: { method: "POST", body: "a=12&b=30",
      headers: { "Content-Type": "application/x-www-form-urlencoded" } }, matches: addition }
  ];

  function addition(text, type) {
    if (!type.startsWith("application/json")) return false;
    const data = JSON.parse(text);
    return data.a === 12 && data.b === 30 && data.sum === 42;
  }

  httpButton.addEventListener("click", async () => {
    httpButton.disabled = true;
    httpTable.replaceChildren();
    let passed = 0;
    for (let index = 0; index < probes.length; index++) {
      const probe = probes[index];
      const row = httpTable.insertRow();
      for (const value of [probe.name, "—", "—", "—", "—", "진행 중"])
        row.insertCell().textContent = value;
      httpSummary.textContent = `HTTP 응답 확인 중 · ${index + 1} / ${probes.length}`;
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), 8000);
      const started = performance.now();
      try {
        const response = await fetch(probe.url, { ...probe.options, cache: "no-store", signal: controller.signal });
        const text = await response.text();
        const type = response.headers.get("Content-Type") || "";
        const ok = response.status === 200 && probe.matches(text, type);
        row.cells[1].textContent = String(response.status);
        row.cells[2].textContent = type || "없음";
        row.cells[3].textContent = `${new TextEncoder().encode(text).length} B`;
        row.cells[5].textContent = ok ? "응답 통과" : "예상 응답 불일치 · CGI 실행 인자도 확인하세요.";
        row.className = ok ? "request-success" : "request-failure";
        if (ok) passed++;
      } catch (error) {
        row.cells[5].textContent = error.name === "AbortError" ? "8초 시간 초과" : `검사 실패: ${error.message}`;
        row.className = "request-failure";
      } finally {
        clearTimeout(timer);
        row.cells[4].textContent = `${(performance.now() - started).toFixed(1)} ms`;
      }
    }
    httpSummary.textContent = `HTTP 응답 통과 ${passed} / 실패 ${probes.length - passed} · TCP 연결 재사용 여부는 별도 실행기로 확인하세요.`;
    httpSummary.dataset.result = passed === probes.length ? "pass" : "fail";
    httpButton.disabled = false;
  });

  if (location.protocol === "file:") {
    httpButton.disabled = true;
    httpSummary.textContent = "HTTP 응답 확인은 실행 중인 서버 주소로 접속해야 합니다.";
  }
})();
