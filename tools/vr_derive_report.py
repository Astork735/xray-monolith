"""One row per weapon from a vr_derive_dump, with likely problems flagged.

In the game console run `vr_derive_dump`, then:

    python tools/vr_derive_report.py F:/Games/stalker2/appdata/logs/xray_andreas.log
    python tools/vr_derive_report.py first.log resumed.log   a dump split by a crash (`vr_derive_dump resume`)
    python tools/vr_derive_report.py LOG --all          every weapon, not only flagged ones
    python tools/vr_derive_report.py LOG --csv out.csv  the full table as CSV

Each log's last dump is read; with several, later logs win for a weapon in both. Flags:
  unsupported      no VR handling (knives, binoculars: expected)
  crashed          loading its section crashed the game (a broken section the game never spawns)
  skipped          not loaded: its HUD model or a motion file does not exist, or a sound
                   entry is empty (a section the game never spawns; loading it would
                   crash the game). The reason is in the notes column.
  no-cycling-part  no bolt or slide found: an invisible charging point stands in
  no-magazine      the mechanics say detachable magazine, the model gave none
  native-reload    the game's own reload: no physical magazine or shells
  no-action        the mechanics found no action (none) or no feed (none)
  override-refused an override key not applied (often an outdated vr_weapon_mechanics.ltx)
  warning          any other '!' line (the text is in the notes column)
"""
import argparse
import csv
import re
import sys
from pathlib import Path

ROW = re.compile(r'^[*!] \[VR [a-z -]+\] (?:vr_wpn_)?(?P<section>[^:]+): (?P<text>.*)$')


def last_dump(path):
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
            weapons[current].append(line)
    return weapons


def describe(name, lines):
    row = dict(weapon=name, profile='', family='', action='', feed='', cycling='', magazine='',
               reload='', flags=[], notes=[])
    for line in lines:
        m = ROW.match(line)
        text = m.group('text') if m else line
        if line.startswith('!'):
            # An override key the derivation refuses (an outdated
            # vr_weapon_mechanics.ltx in the game) is a configuration matter;
            # a section naming files that do not exist is skipped by the dump.
            if ' not applied' in line:
                row['flags'].append('override-refused')
            elif '[VR dump]' in line and 'crashed the game' in line:
                row['flags'].append('crashed')
            elif '[VR dump]' in line and ': skipped: ' in line:
                row['flags'].append('skipped')
            else:
                row['flags'].append('warning')
            row['notes'].append(line[2:140])
        if text.startswith('profile: '):
            row['profile'] = text.split(',')[0][len('profile: '):]
            bone = re.search(r'mag_bone ([^,]*),', text)
            detachable = re.search(r'detachable_magazine (\d)', text)
            row['magazine'] = (bone.group(1).strip() if bone else '') if detachable and detachable.group(1) == '1' else '-'
        elif '[VR mechanics]' in line and re.match(r'[a-z_]+, [a-z_]+, ', text):
            parts = [p.strip() for p in text.split(';')[0].split(',')]
            row['family'], row['action'], row['feed'] = parts[0], parts[1], parts[2]
        elif text.startswith('cycling part '):
            row['cycling'] = text.split(',')[0][len('cycling part '):]
        elif text.startswith('no cycling part found'):
            row['cycling'] = 'none (charging point)'
        elif text.startswith('reload strategy: '):
            row['reload'] = text[len('reload strategy: '):].split(' (')[0]
    if not row['profile']:
        if not {'skipped', 'crashed'} & set(row['flags']):
            row['flags'].append('unsupported')
    else:
        # A revolver or launcher has no bolt to charge (ClassifyMechanics).
        if row['cycling'].startswith('none') and row['family'] not in ('revolver', 'launcher'):
            row['flags'].append('no-cycling-part')
        if row['feed'].startswith('detachable') and row['magazine'] in ('', '-'):
            row['flags'].append('no-magazine')
        if row['reload'] == 'native reload':
            row['flags'].append('native-reload')
        if row['action'] == 'none' or row['feed'] == 'none':
            row['flags'].append('no-action')
    row['flags'] = sorted(set(row['flags']))
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log', type=Path, nargs='+')
    parser.add_argument('--all', action='store_true', help='list every weapon, not only flagged ones')
    parser.add_argument('--csv', type=Path, help='write the full table here')
    args = parser.parse_args()
    weapons = {}
    for log in args.log:
        weapons.update(last_dump(log))
    rows = [describe(name, lines) for name, lines in sorted(weapons.items())]
    columns = ['weapon', 'profile', 'family', 'action', 'feed', 'cycling', 'magazine', 'reload', 'flags', 'notes']
    if args.csv:
        with args.csv.open('w', newline='', encoding='utf-8') as out:
            writer = csv.writer(out)
            writer.writerow(columns)
            for row in rows:
                writer.writerow([' '.join(row[c]) if c == 'flags' else ' | '.join(row[c]) if c == 'notes' else row[c]
                                 for c in columns])
    shown = rows if args.all else [row for row in rows if row['flags']]
    out = sys.stdout
    out.write('| %s |\n|%s\n' % (' | '.join(columns[:-1]), '---|' * (len(columns) - 1)))
    for row in shown:
        out.write('| %s |\n' % ' | '.join(' '.join(row[c]) if c == 'flags' else row[c] for c in columns[:-1]))
    counts = {}
    for row in rows:
        for flag in row['flags']:
            counts[flag] = counts.get(flag, 0) + 1
    out.write('\n%d weapons; %s\n' % (len(rows), ', '.join('%s %d' % kv for kv in sorted(counts.items())) or 'nothing flagged'))
    notes = sorted({note for row in rows for note in row['notes']})
    if notes:
        out.write('warnings:\n' + ''.join('  %s\n' % note for note in notes[:40]))


if __name__ == '__main__':
    main()
