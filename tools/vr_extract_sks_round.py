"""Extract one complete 7.62x39 cartridge from the stock SKS stripper clip.

Keeps the original cartridge material and skeletal vertex format, removes
the weapon, clip and other cartridges, and centres the round for hand use.
The output is a rigid skeleton with no external animation dependencies.
"""
import argparse
import hashlib
import math
import struct
from collections import defaultdict
from pathlib import Path

from vr_extract_pm_magazines import chunks, pack

SOURCE_SHA256 = '95ca17133342310087c13190c323f54c4f073538ba9c8d451158dfa62be95897'
SOURCE = Path('_build/asset-audit/extracted/meshes/anomaly_weapons/wpn_sks/wpn_sks_hud.ogf')
DEST = Path('gamedata/meshes/vr/round_762x39.ogf')


def generate(source, output):
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError('SKS HUD differs from the audited stock model')
    top = dict(chunks(data))
    mesh = dict(chunks(list(chunks(top[9]))[1][1]))
    vertices = mesh[3]
    fmt, count = struct.unpack_from('<II', vertices)
    if fmt != 1 or len(vertices) != 8 + count * 60:
        raise ValueError('unexpected SKS vertex format')
    positions = [struct.unpack_from('<3f', vertices, 8 + i * 60) for i in range(count)]
    bones = [struct.unpack_from('<I', vertices, 8 + i * 60 + 56)[0] for i in range(count)]
    indices = struct.unpack_from(f'<{struct.unpack_from("<I", mesh[4])[0]}H', mesh[4], 4)
    triangles = list(zip(indices[0::3], indices[1::3], indices[2::3]))
    parent = list(range(count))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    # Weld UV/normal seams for connectivity, retaining the original vertices.
    weld = {}
    for i in range(count):
        if bones[i] == 9:  # naboje: the live rounds, not gilza (spent case)
            key = tuple(round(c, 5) for c in positions[i])
            parent[find(i)] = find(weld.setdefault(key, i))
    for a, b, c in triangles:
        if bones[a] == bones[b] == bones[c] == 9:
            parent[find(b)] = find(a)
            parent[find(c)] = find(a)
    groups = defaultdict(list)
    for i in range(count):
        if bones[i] == 9:
            groups[find(i)].append(i)
    if len(groups) != 9 or any(len(g) != 70 for g in groups.values()):
        raise ValueError('expected nine separate complete clip cartridges')
    # The clip fans its rounds: take the one closest to the model's +Z axis.
    used = min(groups.values(), key=lambda g: max(positions[i][1] for i in g) - min(positions[i][1] for i in g))
    lo = [min(positions[i][c] for i in used) for c in range(3)]
    hi = [max(positions[i][c] for i in used) for c in range(3)]
    centre = [(a + b) * 0.5 for a, b in zip(lo, hi)]
    scale = 0.056 / (hi[2] - lo[2])  # complete cartridge length, metres
    remap = {old: new for new, old in enumerate(used)}
    kept = [remap[i] for tri in triangles if all(i in remap for i in tri) for i in tri]
    if not kept:
        raise ValueError('no cartridge triangles')
    raw = bytearray()
    for i in used:
        v = bytearray(vertices[8 + i * 60:8 + (i + 1) * 60])
        struct.pack_into('<3f', v, 0, *((positions[i][c] - centre[c]) * scale for c in range(3)))
        raw.extend(v)
    half = [(b - a) * scale * 0.5 for a, b in zip(lo, hi)]

    def header(kind):
        return struct.pack('<BBH10f', 4, kind, 0, *(-x for x in half), *half,
                           0, 0, 0, math.sqrt(sum(x * x for x in half)))

    child = pack([(1, header(5)), (2, mesh[2]),
                  (3, struct.pack('<II', fmt, len(used)) + raw),
                  (4, struct.pack('<I', len(kept)) + struct.pack(f'<{len(kept)}H', *kept))])
    result = pack([(1, header(10)), (9, pack([(0, child)])), (13, top[13]), (16, top[16])])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result)
    print(f'{output}: {len(used)} vertices, {len(kept)//3} triangles, {len(result)} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--output', type=Path, default=DEST)
    args = parser.parse_args()
    generate(args.source, args.output)
