"""Compares two vr_derive_dump runs: what a code change did to each weapon's derivation.

In the game console run `vr_derive_dump` (optionally with parts of section
names, e.g. `vr_derive_dump ak svd`; `derived` also derives the weapons with an
audited profile), once before a change and once after, keeping a copy of the
log in between. Then:

    python tools/vr_derive_diff.py before.log after.log
    python tools/vr_derive_diff.py --before a.log a2.log --after b.log b2.log   dumps split by a crash

Each run's last dump is read. Timings are dropped; every other difference in
the VR lines is printed per weapon, and weapons only one run measured are listed.
"""
import argparse
import difflib
import re
from pathlib import Path

TIMING = re.compile(r'\b(in|after|took) [0-9.]+ ?ms\b|; [0-9.]+ ms\b|[0-9.]+ ms\)')


def last_dump(path):
    """{section: [lines]} of the log's last complete dump."""
    lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
    starts = [i for i, line in enumerate(lines) if '[VR dump] start:' in line]
    if not starts:
        raise SystemExit('%s: no vr_derive_dump in this log' % path)
    weapons, current = {}, None
    for line in lines[starts[-1] + 1:]:
        if '[VR dump] done:' in line:
            break
        if re.match(r'^\* \[VR dump\] \d+ of \d+, ', line):
            continue  # progress, not a weapon
        if line.startswith('* [VR dump] '):
            current = line[len('* [VR dump] '):].strip()
            weapons[current] = []
        elif current and re.match(r'^[*!] \[VR ', line):
            weapons[current].append(TIMING.sub('<time>', line))
    return weapons


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('logs', type=Path, nargs='*', help='before.log after.log')
    parser.add_argument('--before', type=Path, nargs='+', default=[])
    parser.add_argument('--after', type=Path, nargs='+', default=[])
    args = parser.parse_args()
    if args.logs:
        if len(args.logs) != 2 or args.before or args.after:
            parser.error('give before.log after.log, or --before ... --after ...')
        args.before, args.after = [args.logs[0]], [args.logs[1]]
    if not args.before or not args.after:
        parser.error('nothing to compare')

    def merged(logs):
        weapons = {}
        for log in logs:
            weapons.update(last_dump(log))
        return weapons
    before, after = merged(args.before), merged(args.after)
    # Profile summaries (added to the dump later) count only when both have them.
    def has_profiles(weapons):
        return any(': profile: ' in line for lines in weapons.values() for line in lines)
    if has_profiles(before) != has_profiles(after):
        for weapons in (before, after):
            for name in weapons:
                weapons[name] = [line for line in weapons[name] if ': profile: ' not in line]
    changed = 0
    for name in sorted(set(before) | set(after)):
        if name not in after:
            print('only before: %s' % name)
            continue
        if name not in before:
            print('only after: %s' % name)
            continue
        if before[name] == after[name]:
            continue
        changed += 1
        print('== %s' % name)
        for line in difflib.unified_diff(before[name], after[name], lineterm='', n=0):
            if not line.startswith(('---', '+++', '@@')):
                print('  ' + line)
    same = len(set(before) & set(after)) - changed
    print('%d weapons changed, %d unchanged' % (changed, same))


if __name__ == '__main__':
    main()
