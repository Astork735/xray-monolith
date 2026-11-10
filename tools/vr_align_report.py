"""How the first-person animation's hands and the magazine sit on each weapon, from a
vr_derive_dump, with the weapons that stand out flagged.

The dump logs one `[VR align]` line per weapon (CVrWeaponInteraction::LogAlignment): the
animation's weapon-hand palm and support-hand palm against the mesh, the hand's
orientation on the weapon, and the magazine against the bore's centre plane. Weapons
sharing a HUD section share all of it, so rows are per HUD section.

    python tools/vr_align_report.py F:/Games/stalker2/appdata/logs/xray_andreas.log
    python tools/vr_align_report.py first.log resumed.log    a dump split by a crash
    python tools/vr_align_report.py LOG --all                 every HUD section, not only flagged
    python tools/vr_align_report.py LOG --csv align.csv       the full table as CSV
    python tools/vr_align_report.py LOG --flag hold-turned    only rows with this flag

Flags (thresholds below):
  grip-untrusted  the animation's hand is far from the measured grip: VR holds the weapon
                  by the measured grip and draws the arm by IK (often looks offset)
  grip-off        the animation's palm is 3 cm or more from the measured grip centre
  grip-beside     the palm is 1.5 cm or more aside of the bore's centre plane
  grip-in-air     the palm is not round the mesh and 3 cm or more from it
  hold-turned     the hand is turned 15 degrees or more on the weapon from the usual
                  hold of its class (handgun or long gun): the gun looks turned in the hand
  no-support      a long gun whose idle animation has no support hand on it
  support-beside  the support palm is 2.5 cm or more aside of the bore's centre plane
  support-in-air  the support fist does not close on mesh and is 2 cm or more from it
  mag-beside      the magazine's centre is 5 mm or more aside of the bore's centre plane
  well-tilted     the magazine well leans 3 degrees or more sideways
  no-mag-hold     the reload animation gave no hand pose holding the magazine
  hold-mesh       the animation's hand is not used: the mesh grip holds it, the arm by IK
  hold-default    no mesh grip either: the grip is estimated from the trigger (or a default)
  wrist-lift      the stock-wrist correction raised the grip centre 1 cm or more
  support-line    a long gun's support hand on the controller-to-muzzle line, not on the mesh
  index-off       the animation's hand is used but its index fingertip is 4 cm or more from the
                  trigger (the hold may be on another part of the gun)
  trigger-hand    no trigger found on the model: the animation's index fingertip is used
"""
import argparse
import csv
import math
import re
import statistics
from pathlib import Path

GRIP_OFF = 0.03
GRIP_SIDE = 0.015
GRIP_AIR = 0.03
HOLD_TURN_DEG = 15.0
SUPPORT_SIDE = 0.025
SUPPORT_AIR = 0.02
MAG_SIDE = 0.005
WELL_SIDE_DEG = 3.0
WRIST_LIFT = 0.01

LINE = re.compile(r'^\* \[VR align\] (?P<section>[^:]+): hud=(?P<hud>\S+) (?P<rest>.*)$')


def read(paths):
    """section -> fields, from each log's last dump; later logs win."""
    rows = {}
    for path in paths:
        lines = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
        starts = [i for i, line in enumerate(lines) if '[VR dump] start:' in line]
        if not starts:
            raise SystemExit('%s: no vr_derive_dump in this log' % path)
        found = 0
        for line in lines[starts[-1]:]:
            m = LINE.match(line)
            if not m:
                continue
            fields = dict(item.split('=', 1) for item in m.group('rest').split() if '=' in item)
            fields['hud'] = m.group('hud')
            rows[m.group('section')] = fields
            found += 1
        if not found:
            print('%s: no [VR align] lines (a dump from a build before LogAlignment?)' % path)
    return rows


def vec(text):
    return [float(x) for x in text.split(',')]


def angle(a, b):
    dot = sum(x * y for x, y in zip(a, b))
    la = math.sqrt(sum(x * x for x in a))
    lb = math.sqrt(sum(x * x for x in b))
    if la < 1e-6 or lb < 1e-6:
        return 0.0
    return math.degrees(math.acos(max(-1.0, min(1.0, dot / (la * lb)))))


def median_dir(dirs):
    if not dirs:
        return None
    m = [statistics.median(d[i] for d in dirs) for i in range(3)]
    n = math.sqrt(sum(x * x for x in m)) or 1.0
    return [x / n for x in m]


def assess(rows):
    # The usual hold per class: the median of the hands' directions on the weapon.
    usual = {}
    for cls in ('handgun', 'long'):
        held = [r for r in rows.values() if r.get('class') == cls and 'knuckles_dir' in r]
        usual[cls] = (median_dir([vec(r['knuckles_dir']) for r in held]),
                      median_dir([vec(r['palm_normal']) for r in held]))
    out = []
    for section, r in rows.items():
        flags, f = [], lambda key: float(r[key]) if key in r else None
        turn = None
        if r.get('palm', 'none') != 'none':
            if r.get('trusted') == '0':
                flags.append('grip-untrusted')
            if f('palm_grip') >= GRIP_OFF:
                flags.append('grip-off')
            if abs(f('palm_side')) >= GRIP_SIDE:
                flags.append('grip-beside')
            if r.get('palm_round') == '0' and f('palm_mesh') >= GRIP_AIR:
                flags.append('grip-in-air')
            knuckles, normal = usual[r['class']]
            if knuckles and normal:
                turn = max(angle(vec(r['knuckles_dir']), knuckles), angle(vec(r['palm_normal']), normal))
                if turn >= HOLD_TURN_DEG:
                    flags.append('hold-turned')
        if r.get('support', 'none') == 'none':
            if r.get('class') == 'long':
                flags.append('no-support')
        elif r.get('rear') != '1':
            if abs(f('support_side')) >= SUPPORT_SIDE:
                flags.append('support-beside')
            if r.get('support_closes') == '0' and f('support_mesh') >= SUPPORT_AIR:
                flags.append('support-in-air')
        if 'mag_side' in r:
            if abs(f('mag_side')) >= MAG_SIDE:
                flags.append('mag-beside')
            if abs(f('well_side_deg')) >= WELL_SIDE_DEG:
                flags.append('well-tilted')
            if r.get('mag_hold') == '0':
                flags.append('no-mag-hold')
        if 'hold_src' in r:
            src = r['hold_src']
            if src.startswith('mesh'):
                flags.append('hold-mesh')
            elif src in ('trigger_estimate', 'default'):
                flags.append('hold-default')
            if abs(f('wrist_lift')) >= WRIST_LIFT:
                flags.append('wrist-lift')
            if r.get('class') == 'long' and r.get('support_src') == 'line':
                flags.append('support-line')
            if src.startswith('anim') and (f('index_trigger') or -1) >= 0.04:
                flags.append('index-off')
            if r.get('trigger_from_hand') == '1':
                flags.append('trigger-hand')
        mm = lambda key: '%.0f' % (f(key) * 1000) if key in r else ''
        out.append(dict(section=section, hud=r['hud'], cls=r.get('class', ''), flags=flags,
                        palm_grip_mm=mm('palm_grip'), palm_side_mm=mm('palm_side'),
                        hold_turn_deg='' if turn is None else '%.0f' % turn,
                        support_side_mm=mm('support_side'), support_mesh_mm=mm('support_mesh'),
                        mag_side_mm=mm('mag_side'), well_side_deg=r.get('well_side_deg', ''),
                        hold_src=r.get('hold_src', ''), lift_mm=mm('wrist_lift'),
                        support_src=r.get('support_src', '')))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('logs', nargs='+')
    parser.add_argument('--all', action='store_true')
    parser.add_argument('--csv')
    parser.add_argument('--flag')
    args = parser.parse_args()
    rows = assess(read(args.logs))
    # One row per HUD section: its weapons share the model, animations and hold.
    by_hud = {}
    for row in rows:
        by_hud.setdefault(row['hud'], []).append(row)
    table = []
    for hud, members in sorted(by_hud.items()):
        first = dict(members[0])
        first['weapons'] = len(members)
        first['sections'] = ' '.join(sorted(m['section'] for m in members))
        table.append(first)
    counts = {}
    for row in table:
        for flag in row['flags']:
            counts[flag] = counts.get(flag, 0) + 1
    print('%d weapons, %d HUD sections' % (len(rows), len(table)))
    for flag, n in sorted(counts.items(), key=lambda x: -x[1]):
        print('  %-15s %4d HUD sections' % (flag, n))
    columns = ['hud', 'weapons', 'cls', 'flags', 'palm_grip_mm', 'palm_side_mm', 'hold_turn_deg',
               'support_side_mm', 'support_mesh_mm', 'mag_side_mm', 'well_side_deg', 'hold_src',
               'lift_mm', 'support_src', 'sections']
    if args.csv:
        with open(args.csv, 'w', newline='', encoding='utf-8') as out:
            writer = csv.DictWriter(out, columns, extrasaction='ignore')
            writer.writeheader()
            for row in table:
                writer.writerow(dict(row, flags=' '.join(row['flags'])))
        print('wrote', args.csv)
    shown = [r for r in table if (args.all or r['flags']) and (not args.flag or args.flag in r['flags'])]
    if shown:
        print()
        print('%-34s %4s %-7s %5s %5s %5s %6s %6s %5s %5s %-20s  %s' % (
            'hud', 'n', 'class', 'grip', 'side', 'turn', 'sup', 'supgap', 'mag', 'well', 'hold', 'flags'))
        for r in shown:
            print('%-34s %4d %-7s %5s %5s %5s %6s %6s %5s %5s %-20s  %s' % (
                r['hud'][:34], r['weapons'], r['cls'], r['palm_grip_mm'], r['palm_side_mm'], r['hold_turn_deg'],
                r['support_side_mm'], r['support_mesh_mm'], r['mag_side_mm'], r['well_side_deg'],
                r['hold_src'][:20], ' '.join(r['flags'])))


if __name__ == '__main__':
    main()
