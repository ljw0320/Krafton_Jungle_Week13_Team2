"""Build a Korean report and standalone SVG from the recorded trials."""
import json
import csv
from pathlib import Path
import statistics

BASE = Path(__file__).resolve().parent
OUT = BASE/'results'
rows = [json.loads(line) for line in (OUT/'measurements.jsonl').read_text().splitlines()]
extra = OUT/'nuri-high.jsonl'
if extra.exists():
    rows += [json.loads(line) for line in extra.read_text().splitlines()]
    executed = {(r['branch'], r['target'], r['repeat']) for r in rows if 'successes' in r}
    rows = [r for r in rows if 'successes' in r or (r['branch'],r['target'],r['repeat']) not in executed]
fields = list(dict.fromkeys(key for row in rows for key in row))
with (OUT/'combined.csv').open('w', newline='') as file:
    writer = csv.DictWriter(file, fieldnames=fields)
    writer.writeheader()
    writer.writerows(rows)
groups = {}
for row in rows:
    groups.setdefault((row['branch'], row['target']), []).append(row)

environment = json.loads((OUT/'measurements.jsonl.environment.json').read_text())
cpu_model = next((line.split(':', 1)[1].strip() for line in environment['cpu'].splitlines() if line.startswith('model name')), 'unknown')
logical_cpus = sum(line.startswith('processor') for line in environment['cpu'].splitlines())

lines = ['# 로컬 C10K 연결 유지 실험 결과', '',
         f'환경: {cpu_model}, WSL 논리 CPU {logical_cpus}개. 실행 커널: `{environment["platform"]}`.', '',
         '**환경 제한:** systemd init.scope의 TasksMax=9392는 변경하지 않았다. nuri의 10000 목표는 이 작업 수 한도에서 fork EAGAIN으로 막혔다. 스레드 풀/epoll의 연결 수는 같은 수의 실행 주체를 요구하지 않는다. 근거: [한도 기록](system-limits.txt), `nuri-10000-r*.log`. 이 환경의 결과를 모델의 보편적인 수용 한도로 해석하지 않는다.', '',
         '같은 WSL에서 같은 1KiB 파일, 유휴 시간 600초, O2 빌드로 측정했다. 정상 요청은 초당 10회, 각 조건 8초 × 3회다. 아래 지연은 **각 회차 p95의 중앙값**이며 240개 요청을 합친 p95가 아니다. 메모리는 측정 직전 PSS 합계의 중앙값이다.', '',
         '| 브랜치 | 목표 연결 | 실제 유지 전/후 | 회차 | 프로세스/스레드 | PSS MiB | 정상 요청 성공/전체 | p95 중앙값 ms (회차 범위) | CPU % 중앙값 |',
         '|---|---:|---|---:|---|---:|---:|---|---:|']
summary = []
for (branch, target), trials in sorted(groups.items()):
    valid = [r for r in trials if 'successes' in r]
    if not valid:
        lines.append(f'| {branch} | {target} | 미실행: 프로세스 안전 제한 | 0 | — | — | — | — | — |')
        continue
    p95 = [r['p95_ms'] for r in valid if r['p95_ms'] is not None]
    med = lambda key: statistics.median(r[key] for r in valid)
    held = ', '.join(f"{r['held_before']}/{r['held_after']}" for r in valid)
    succeeded = sum(r['successes'] for r in valid)
    total = sum(r['requests'] for r in valid)
    delay = f'{statistics.median(p95):.2f} ({min(p95):.2f}–{max(p95):.2f})' if p95 else '—'
    lines.append(f'| {branch} | {target} | {held} | {len(valid)} | {med("processes"):g}/{med("threads"):g} | {med("pss_kib")/1024:.2f} | {succeeded}/{total} | {delay} | {med("server_cpu_percent"):.2f} |')
    summary.append(dict(branch=branch, target=target, pss_mib=med('pss_kib')/1024,
                        held=med('held_before'),
                        cpu_percent=med('server_cpu_percent'),
                        p95_ms=statistics.median(p95) if p95 else None,
                        success=succeeded, total=total, maintained=all(r['status']=='ok' for r in valid)))
lines += ['', '## 해석 범위', '',
          '- 기본 실행의 nuri 3000/10000 미실행 항목은 추가 실행 결과가 있을 때 대체한다. 추가 실행 환경은 nuri-high.jsonl.environment.json 참조.',
          '- 0연결 메모리에는 각 구현의 고정 할당이 포함된다. 증가량도 함께 비교해야 한다.',
          '- 이 부하는 포화 처리량 측정이 아니다. 지연으로 모델의 일반적인 속도 순위를 매길 수 없다.',
          '- PSS는 사용자 공간 메모리이며 커널 소켓·프로세스 유지 비용 전체가 아니다.',
          '- 같은 WSL의 부하 발생기와 Windows 마운트 파일/로그 I/O가 결과에 영향을 준다.',
          '- EOF 미관찰로 확인한 전후 연결 수이며 측정 중 전체 연결의 연속 생존 검증은 아니다.',
          '- 실험용 조건 변경과 측정 정의는 상위 README 및 패치 파일을 참조한다.', '',
          '## 원자료', '', '[CSV](measurements.csv) · [JSONL](measurements.jsonl) · [커밋](commits.json) · [환경](measurements.jsonl.environment.json)', '',
          '[통합 CSV](combined.csv) · 추가 실행 원자료가 있으면 `nuri-high.jsonl`에 보관.', '',
          '![결과 그래프](comparison.svg)', '']
lines += ['## 발표에서 사용할 수 있는 관찰', '',
          '1. 연결 수와 실행 주체 수는 같은 개념이 아니다. 실제 프로세스·스레드 수 열로 확인한다.',
          '2. nuri의 연결별 프로세스 유지, shw의 연결별 구조체/버퍼 유지, ljw의 사전 할당 연결 슬롯 및 고정 context 풀이 서로 다른 메모리 곡선을 만든다.',
          '3. ljw의 낮은 추가 PSS는 대기 연결이 무료라는 뜻이 아니다. 12000개 연결 슬롯을 위한 고정 할당이 0연결 기준에도 포함되고, 커널 소켓 비용은 이 표에서 빠진다.',
          '4. poll의 감시 비용과 epoll의 이벤트 기반 처리는 CPU 변화의 설명 후보다. CPU 수치에는 타이머 순회·로그·요청 처리 등도 포함되므로 poll 하나의 비용으로 단정하지 않는다.',
          '5. 성공한 조건은 “다수 연결을 유지하며 초당 10개 정상 요청을 처리했다”는 증거다. 모든 연결의 동시 요청 처리나 최대 처리량을 증명하지 않는다.', '']
(OUT/'REPORT.md').write_text('\n'.join(lines))
(OUT/'summary.json').write_text(json.dumps(summary, indent=2))

from plot_comparison import draw
draw(summary, OUT)
print('\n'.join(lines[:25]))
