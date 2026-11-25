"""Check completeness and cleanup of this experiment's saved results."""
import csv
from pathlib import Path
import xml.etree.ElementTree as ET

base = Path(__file__).resolve().parent
rows = list(csv.DictReader((base/'results'/'combined.csv').open()))
expected = {(branch, str(level), str(repeat))
            for branch in ('nuri', 'ljw', 'shw')
            for level in (0, 100, 500, 1000, 3000, 10000)
            for repeat in (1, 2, 3)}
actual = {(row['branch'], row['target'], row['repeat']) for row in rows}
assert actual == expected and len(rows) == len(expected), 'missing or duplicated trial'
for row in rows:
    assert int(row['requests']) == 80
    assert int(row['successes']) + int(row['failures']) == 80
    if row['status'] == 'ok':
        assert row['held_before'] == row['held_after'] == row['target']
ET.parse(base/'results'/'comparison.svg')
leftovers = []
for process in Path('/proc').iterdir():
    if not process.name.isdigit():
        continue
    try:
        if (process/'exe').resolve().is_relative_to(base/'sources'):
            leftovers.append(process.name)
    except (OSError, RuntimeError):
        pass
assert not leftovers, f'server processes remain: {leftovers}'
successes = sum(int(row['successes']) for row in rows)
requests = sum(int(row['requests']) for row in rows)
print(f'PASS: {len(rows)} trials, {successes}/{requests} successful probes; valid SVG; no experiment servers remain.')
