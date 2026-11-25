"""Presentation SVG with large type, direct labels and two distinct questions."""
from html import escape


def draw(summary, output):
    colors = {'nuri': '#c34b39', 'ljw': '#2468c4', 'shw': '#15806e'}
    names = {'nuri': '프로세스', 'ljw': '스레드 풀', 'shw': 'epoll'}
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1600" height="900" viewBox="0 0 1600 900" role="img" aria-labelledby="title desc">',
             '<title id="title">대기 연결 증가에 따른 서버 메모리와 CPU 비용</title>',
             '<desc id="desc">실제 유지한 연결 수를 가로축으로 사용한다. 프로세스형은 3000개까지 새 요청 성공을 확인했고 9382개 유지 상태에서 새 요청이 실패했다. 그 사이 구간은 측정하지 않았다. 실패 상태의 메모리는 별도 X 표시하며 CPU는 같은 작업을 수행하지 못했으므로 비교에서 제외한다.</desc>',
             '<rect width="1600" height="900" fill="#fafbfd"/>',
             '<style>text{font-family:"Malgun Gothic","Noto Sans CJK KR",sans-serif;fill:#172b42;font-size:24px} .muted{fill:#52657b} .heading{font-size:42px;font-weight:700} .panel{font-size:30px;font-weight:700} .value{font-size:24px;font-weight:700}</style>']

    def text(x, y, value, cls='', color=None, anchor=None):
        attrs = f' class="{cls}"'
        if color:
            attrs += f' style="fill:{color}"'
        if anchor:
            attrs += f' text-anchor="{anchor}"'
        parts.append(f'<text x="{x}" y="{y}"{attrs}>{escape(str(value))}</text>')

    text(64, 66, 'C10K: 대기 연결의 비용과 새 요청 수용', 'heading')
    text(64, 108, '동일 WSL · 정상 요청 10회/초 · 조건별 3회 중앙값', 'muted')
    for left in (48, 824):
        parts.append(f'<rect x="{left}" y="140" width="728" height="552" rx="18" fill="white" stroke="#e0e6ee"/>')

    for key, x0, xmax, ymax, title, unit, xticks, yticks in [
            ('pss_mib', 116, 10000, 1200, '연결을 보관하는 메모리', '메모리 · MiB', [0,1000,3000,10000], [0,400,800,1200]),
            ('cpu_percent', 898, 10000, 40, '연결을 관리하는 CPU', 'CPU · %', [0,1000,3000,10000], [0,10,20,30,40])]:
        width, top, height = 448, 222, 390
        text(x0-38, 182, title, 'panel')
        text(x0, 213, unit, 'muted')
        for tick in yticks:
            y = top+height-height*tick/ymax
            parts.append(f'<path d="M{x0},{y}h{width}" stroke="#e3e9f0" stroke-width="1.5"/>')
            text(x0-14, y+8, tick, 'muted', anchor='end')
        for tick in xticks:
            x = x0+width*tick/xmax
            label = f'{tick:,}'
            parts.append(f'<path d="M{x},{top+height}v7" stroke="#91a0b2"/>')
            parts.append(f'<text x="{x}" y="{top+height+34}" class="muted" text-anchor="middle" style="font-size:20px">{label}</text>')
        text(x0+width/2, 678, '유지한 대기 연결 수', 'muted', anchor='middle')
        for branch, color in colors.items():
            data = sorted((r for r in summary if r['branch']==branch and r['target']<=xmax and r['maintained']), key=lambda r:r['target'])
            points = [(x0+width*r['held']/xmax, top+height-height*r[key]/ymax) for r in data]
            coords = ' '.join(f'{x:.2f},{y:.2f}' for x,y in points)
            dash = ' stroke-dasharray="10 7"' if branch == 'shw' else ''
            parts.append(f'<polyline points="{coords}" fill="none" stroke="{color}" stroke-width="4"{dash}/>')
            for x,y in points:
                parts.append(f'<circle cx="{x}" cy="{y}" r="5.5" fill="white" stroke="{color}" stroke-width="3"/>')
            x,y = points[-1]
            value = data[-1][key]
            label_x = x+18
            label_y = y+8
            if key == 'pss_mib':
                label_y += {'nuri': 0, 'shw': -12, 'ljw': 18}[branch]
            elif branch == 'nuri':
                label_y -= 46
            elif branch == 'shw':
                label_y += 14
            parts.append(f'<path d="M{x+6},{y}L{label_x-4},{label_y-8}" stroke="{color}" stroke-width="1.5"/>')
            text(label_x, label_y, f'{names[branch]} {value:.1f}', 'value', color)
            if branch == 'nuri':
                parts.append(f'<text x="{label_x}" y="{label_y+27}" style="font-size:20px;fill:{color}">3,000개: 새 요청 성공</text>')
        failed = next(r for r in summary if r['branch']=='nuri' and not r['maintained'])
        failed_x = x0+width*failed['held']/xmax
        if key == 'pss_mib':
            failed_y = top+height-height*failed[key]/ymax
            parts.append(f'<path d="M{failed_x-8},{failed_y-8}l16,16m-16,0l16,-16" stroke="{colors["nuri"]}" stroke-width="4"/>')
            text(failed_x+18, failed_y+8, f'{failed[key]:,.1f}', 'value', colors['nuri'])
            text(x0+width+168, top+85, '9,382개 유지 · 새 요청 실패', 'value', colors['nuri'], anchor='end')
        else:
            # Failed probes do no equivalent work: do not plot their CPU as comparable.
            text(x0+width+168, top+190, '9,382개: 새 요청 실패', 'value', colors['nuri'], anchor='end')
            text(x0+width+168, top+220, '실패 상태의 CPU는 비교 제외', 'muted', anchor='end')

    parts.append('<rect x="48" y="718" width="1504" height="130" rx="16" fill="#edf2f8"/>')
    text(76, 758, '3,000개는 마지막 성공 측정 지점입니다. 수용 한계가 아닙니다.', 'value')
    text(76, 798, '프로세스형은 9,382개 연결을 유지한 상태에서 새 요청이 실패했습니다. 중간 구간은 미측정입니다.', 'muted')
    text(64, 882, '대기 중심 부하의 결과입니다 · 무거운 연산·블로킹 작업·격리 효과는 미측정 · CPU 100% = 코어 1개', 'muted')
    parts.append('</svg>')
    (output/'comparison.svg').write_text('\n'.join(parts), encoding='utf-8')
