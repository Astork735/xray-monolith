"""Offline check of the runtime VR weapon measurement on installed HUD models.

Mirrors CVrWeaponInteraction::AutoProfile, DetectShells and MeasureGeometry:
trigger, hand grip (centre, rake, radius), magazine well axis and hold point,
and the shotgun mechanism (break-action hinge or tube port and pump). Use it
to see what the game will derive for stock or modded weapons, for example:

    python tools/vr_measure_weapons.py --game F:/Games/stalker2 wpn_ak74_hud wpn_bm16_hud

Arguments are substrings of archived mesh paths; loose .ogf files may also be
passed with --file. Requires the archive decoder from vr_audit_pm.
"""
import argparse
import math
import struct
from pathlib import Path

from vr_audit_pm import Archive, build_decoder, chunks

# OGF skinned vertex formats: 1-4 links (legacy codes and their aliases).
FORMATS = {1: 1, 0x12071980: 1, 2: 2, 0x240e3300: 2, 3: 3, 0x260e3300: 3, 4: 4, 0x280e3300: 4}


def load(data):
    """Bones with local bind position and the points they dominate."""
    top = {k: data[p:p + n] for k, p, n in chunks(data)}
    table = top[13]
    cursor = 4
    bones = []
    for _ in range(struct.unpack_from('<I', table)[0]):
        end = table.index(0, cursor); name = table[cursor:end].decode(); cursor = end + 1
        end = table.index(0, cursor); parent = table[cursor:end].decode(); cursor = end + 61
        bones.append(dict(name=name, parent=parent, pts=[]))
    ik = top[16]
    cursor = 0
    for bone in bones:
        version = struct.unpack_from('<I', ik, cursor)[0]
        cursor = ik.index(0, cursor + 4) + 1 + 112 + 72 + (4 if version else 0)
        bone['pos'] = struct.unpack_from('<3f', ik, cursor + 12)
        cursor += 40
    for _, offset, size in chunks(top.get(9, b'')):
        child = top[9][offset:offset + size]
        vertices = next((child[p:p + n] for k, p, n in chunks(child) if k == 3), None)
        if vertices is None:
            continue
        fmt, count = struct.unpack_from('<II', vertices)
        links = FORMATS.get(fmt)
        cursor = 8
        for _ in range(count):
            if links == 1:
                point = struct.unpack_from('<3f', vertices, cursor)
                bone = struct.unpack_from('<I', vertices, cursor + 56)[0]; cursor += 60
            elif links == 2:
                b0, b1 = struct.unpack_from('<HH', vertices, cursor)
                point = struct.unpack_from('<3f', vertices, cursor + 4)
                bone = b1 if struct.unpack_from('<f', vertices, cursor + 52)[0] > 0.5 else b0; cursor += 64
            elif links == 3:
                ids = struct.unpack_from('<3H', vertices, cursor)
                point = struct.unpack_from('<3f', vertices, cursor + 6)
                w = struct.unpack_from('<2f', vertices, cursor + 54)
                weights = [w[0], w[1], 1 - w[0] - w[1]]
                bone = ids[weights.index(max(weights))]; cursor += 70
            elif links == 4:
                ids = struct.unpack_from('<4H', vertices, cursor)
                point = struct.unpack_from('<3f', vertices, cursor + 8)
                w = struct.unpack_from('<3f', vertices, cursor + 56)
                weights = [w[0], w[1], w[2], 1 - sum(w)]
                bone = ids[weights.index(max(weights))]; cursor += 76
            else:
                raise ValueError(f'unsupported vertex format {fmt:#x}')
            if bone < len(bones):
                bones[bone]['pts'].append(point)
    return bones


def has(name, part):
    return part in name.lower()


def shell_name(name):
    return any(has(name, p) for p in ('cartridge', 'patron', 'shell', 'gilza', 'bullet'))


def measure(bones, shotgun):
    by = {b['name']: b for b in bones}
    pos = {}
    for b in bones:
        parent = pos.get(b['parent'], (0, 0, 0))
        pos[b['name']] = tuple(a + c for a, c in zip(b['pos'], parent))

    def descends(name, root):
        while name:
            if name == root:
                return True
            name = by[name]['parent']
        return False

    def subtree(root):
        return [p for b in bones if descends(b['name'], root) for p in b['pts']]

    def box(pts):
        return [min(p[i] for p in pts) for i in range(3)], [max(p[i] for p in pts) for i in range(3)]

    out = {}
    trigger = next((b['name'] for b in bones if b['name'] == 'trigger'), None) or next(
        (b['name'] for b in bones if 'trigger' in b['name'] and '_gl' not in b['name']), None)
    t = None
    if trigger:
        pts = by[trigger]['pts']
        t = [(a + b) / 2 for a, b in zip(*box(pts))] if pts else list(pos[trigger])
        out['trigger'] = '(%.3f %.3f %.3f)' % tuple(t)
    muzzle = list(pos.get('muzzle', (0, 0, 0)))
    # Magazine by name, else the part hanging below the receiver ahead of the trigger.
    mag = next((b['name'] for b in bones if has(b['name'], 'magaz') or has(b['name'], 'wpn_mag')
                or b['name'] == 'mag' or b['name'].startswith('mag_')), None)
    if not mag and t:
        total = sum(len(b['pts']) for b in bones)
        for b in bones:
            pts = subtree(b['name']) if b['parent'] else []
            if not pts or len(pts) * 5 > total * 2 or shell_name(b['name']):
                continue
            lo, hi = box(pts)
            centre = (lo[2] + hi[2]) / 2
            if lo[1] < t[1] - 0.06 and t[2] < centre < t[2] + 0.25 and hi[1] - lo[1] > 0.05 and hi[0] - lo[0] < 0.06:
                mag = b['name']
                break
    detachable = False
    if mag and subtree(mag):
        lo, hi = box(subtree(mag))
        size = [h - l for l, h in zip(lo, hi)]
        detachable = 0.03 < size[1] < 0.45 and size[0] < 0.12 and size[2] < 0.3
    # 5 mm voxel cloud without magazine, shells and attachments.
    seen, cloud = set(), []
    for b in bones:
        skip = detachable and descends(b['name'], mag)
        name = b['name']
        while name and not skip:
            skip = shell_name(name) or any(has(name, p) for p in ('scope', 'silencer', 'launcher', 'grenade'))
            name = by[name]['parent']
        if skip:
            continue
        for p in b['pts']:
            key = tuple(math.floor(v * 200) for v in p)
            if key not in seen:
                seen.add(key)
                cloud.append(p)
    out['cloud'] = f'{len(cloud)} points'
    if t:
        region = [p for p in cloud if t[1] - 0.12 < p[1] < t[1] - 0.02 and t[2] - 0.16 < p[2] < t[2] - 0.03
                  and abs(p[0]) < 0.045]
        if len(region) >= 8:
            centre = [sum(p[i] for p in region) / len(region) for i in range(3)]
            upper = [p for p in region if p[1] > centre[1]]
            lower = [p for p in region if p[1] <= centre[1]]
            rake = 18.0
            if upper and lower:
                dy = sum(p[1] for p in upper) / len(upper) - sum(p[1] for p in lower) / len(lower)
                dz = sum(p[2] for p in upper) / len(upper) - sum(p[2] for p in lower) / len(lower)
                measured = math.degrees(math.atan2(dz, dy))
                rake = measured if 0 <= measured <= 45 else 18.0
            spread = sorted(abs(p[0] - centre[0]) for p in region)
            radius = min(max(spread[int(0.9 * (len(spread) - 1))], 0.008), 0.03)
            out['grip'] = 'centre=(%.3f %.3f %.3f) rake=%.0fdeg radius=%.3f' % (*centre, rake, radius)
        else:
            out['grip'] = 'not measured; trigger-based estimate is used'
    if detachable:
        pts = subtree(mag)
        lo, hi = box(pts)
        h = hi[1] - lo[1]
        bottom = [p for p in pts if p[1] <= lo[1] + h * 0.15]
        top = [p for p in pts if p[1] >= hi[1] - h * 0.15]
        b = [sum(p[i] for p in bottom) / len(bottom) for i in range(3)]
        tp = [sum(p[i] for p in top) / len(top) for i in range(3)]
        axis = [a - c for a, c in zip(tp, b)]
        length = math.sqrt(sum(v * v for v in axis))
        axis = [v / length for v in axis]
        if axis[1] < 0.8:
            axis = [0, 1, 0]
        base = [bv - a * (b[1] - lo[1]) / axis[1] for bv, a in zip(b, axis)]
        hold = [bv + a * min(0.04, 0.35 * h / axis[1]) for bv, a in zip(base, axis)]
        out['magazine'] = '%s well axis=(%.2f %.2f %.2f) tilt=%.0fdeg base=(%.3f %.3f %.3f) hold=(%.3f %.3f %.3f)' % (
            mag, *axis, math.degrees(math.acos(axis[1])), *base, *hold)
    if shotgun and not detachable:
        mechanism = None
        for hinge in bones:
            if not hinge['parent']:
                continue
            shells = [b['name'] for b in bones if b['parent'] == hinge['name'] and shell_name(b['name'])
                      and subtree(b['name'])]
            pts = subtree(hinge['name'])
            if shells and pts and box(pts)[1][2] >= muzzle[2] - 0.12:
                mechanism = 'break-action hinge=%s at (%.3f %.3f %.3f) shells=%s' % (
                    hinge['name'], *pos[hinge['name']], shells[:2])
                break
        if not mechanism:
            shell = next((b['name'] for b in bones if shell_name(b['name']) and b['pts']
                          and not has(b['name'], 'gilza')), None)
            pump = next((b['name'] for b in bones if any(has(b['name'], p) for p in
                                                         ('pomp', 'pump', 'forend', 'cevie'))), None)
            for b in bones:
                if pump or not t or not b['parent'] or not b['pts']:
                    continue
                if any(has(b['name'], p) for p in ('scope', 'silencer', 'launcher')) or shell_name(b['name']):
                    continue
                lo, hi = box(b['pts'])
                if lo[2] > t[2] + 0.04 and hi[2] - lo[2] > 0.12 and hi[2] < muzzle[2]:
                    pump = b['name']
            port = '(%.3f %.3f %.3f)' % (t[0], t[1] - 0.012, t[2] + 0.07) if t else None
            mechanism = f'tube shell={shell} pump={pump} port={port}'
        out['shells'] = mechanism
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game', type=Path)
    parser.add_argument('--vcvars', type=Path)
    parser.add_argument('--file', type=Path, action='append', default=[])
    parser.add_argument('--shotgun', action='store_true', help='also detect shell loading for every model')
    parser.add_argument('names', nargs='*', help='substrings of archived HUD mesh paths')
    args = parser.parse_args()
    models = [(str(path), path.read_bytes()) for path in args.file]
    if args.names:
        decoder = build_decoder(args.vcvars)
        archives = sorted((args.game / 'db/meshes').glob('*.db*'))
        for folder in ('patches', 'mods', 'addons'):
            root = args.game / 'db' / folder
            if root.exists():
                archives.extend(sorted(root.rglob('*.db*')))
        for path in archives:
            archive = Archive(path, decoder)
            for key in archive.entries:
                if key.lower().endswith('.ogf') and any(n.lower() in key.lower() for n in args.names):
                    models.append((key, archive.read(key)))
    shotgun_names = ('bm16', 'toz34', 'remington', 'mp133', 'spas', 'protecta', 'saiga', 'mossberg',
                     'mp153', 'fort500', 'toz194', 'wincheaster', 'vepr12', 'usas')
    for name, data in models:
        print('==', name)
        try:
            result = measure(load(data), args.shotgun or any(s in name.lower() for s in shotgun_names))
        except ValueError as error:
            print('   skipped:', error)
            continue
        for key, value in result.items():
            print('   %-9s %s' % (key, value))


if __name__ == '__main__':
    main()
