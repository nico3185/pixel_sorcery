#!/usr/bin/env python3
"""cover_table.py [TITLE ...]: instruction coverage now (games/T/build/kpi.json, from kpi.py) against
a baseline line of docs/kpi/history.csv (default: each title's last line from another commit;
--base SHA picks one). Markdown, one row per title: instructions converted, converted and
executed in verified replays (KPI A), executed but not converted (B), strong suspect gaps (E),
functions never entered.
"""
import csv
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FOCUS = ['sh1', 'sh2', 'sh3', 'shk', 'sh3k', 'shk2', 'shk1', 'coi',
         'qs', 'woi-us']


def main():
    args = sys.argv[1:]
    base_sha = None
    if '--base' in args:
        i = args.index('--base')
        base_sha = args[i + 1]
        del args[i:i + 2]
    titles = args or FOCUS
    head = subprocess.run(['git', '-C', REPO, 'rev-parse', '--short', 'HEAD'], capture_output=True,
                          text=True).stdout.strip()
    base = {}
    for r in csv.DictReader(open(os.path.join(REPO, 'docs', 'kpi', 'history.csv'))):
        if (base_sha and r['git_sha'].startswith(base_sha)) or (not base_sha and r['git_sha'] != head):
            base[r['title']] = r
    print('| title | converted | executed (A) | B | E strong | never entered |')
    print('|---|---|---|---|---|---|')
    for t in titles:
        p = os.path.join(REPO, 'games', t, 'build', 'kpi.json')
        if not os.path.exists(p):
            continue
        k = json.load(open(p))
        b = base.get(t, {})

        def cell(now, was, fmt='{:,}'):
            if was in (None, ''):
                return fmt.format(now)
            return f"{fmt.format(type(now)(float(was)))} -> {fmt.format(now)}"
        print(f"| {t} | {cell(k['converted'], b.get('converted'))} | "
              f"{cell(k['executed_converted'], b.get('executed_converted'))} "
              f"({b.get('coverage_pct', '?')}% -> {k['coverage_pct']:.2f}%) | "
              f"{cell(k['interpreted_only'], b.get('interpreted_only'))} | "
              f"{cell(k.get('suspects_strong', 0), b.get('suspects_strong'))} | "
              f"{cell(k['functions_never_entered'], b.get('functions_never_entered'))} |")


if __name__ == '__main__':
    main()
