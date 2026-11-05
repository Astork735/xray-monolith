"""Extract stock PM magazine meshes with 0..8 visible rounds from its audited HUD OGF.

The stock model uses rigid one-bone triangles. Each cartridge is a separate
connected component at one of eight distinct heights; the script keeps the
magazine, spring and follower and includes only the requested top cartridges.
No weapon geometry is copied into the output meshes.
"""
import argparse
import hashlib
import struct
from collections import defaultdict
from pathlib import Path

SOURCE_SHA256 = "689756bd318854c12f5dabf4eec2b674072cd78a71492162d2d095314306bf7a"
SOURCE = Path("_build/asset-audit/extracted/meshes/anomaly_weapons/wpn_pm/wpn_pm_hud.ogf")
DEST = Path("gamedata/meshes/vr")
MAGAZINE_BONES = {4, 6, 8}
ROUND_BONE = 1
VERTEX_SIZE = 60


def chunks(data):
    offset = 0
    while offset + 8 <= len(data):
        kind, size = struct.unpack_from("<II", data, offset)
        end = offset + 8 + size
        if end > len(data):
            raise ValueError("truncated OGF chunk")
        yield kind, data[offset + 8:end]
        offset = end
    if offset != len(data):
        raise ValueError("trailing OGF bytes")


def pack(chunks_to_write):
    return b"".join(struct.pack("<II", kind, len(data)) + data
                    for kind, data in chunks_to_write)


def generate(source, output):
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != SOURCE_SHA256:
        raise ValueError(f"PM HUD asset differs from audited stock model: {digest}")
    top = list(chunks(data))
    children = list(chunks(dict(top)[9]))
    if len(children) != 2:
        raise ValueError("expected stock PM main mesh plus suppressor mesh")
    main = list(chunks(children[0][1]))
    parts = dict(main)
    vertices, indices = parts[3], parts[4]
    fmt, vertex_count = struct.unpack_from("<II", vertices)
    index_count = struct.unpack_from("<I", indices)[0]
    if fmt != 0x12071980 or len(vertices) != 8 + vertex_count * VERTEX_SIZE or index_count % 3:
        raise ValueError("unexpected PM vertex format")
    raw_indices = struct.unpack_from(f"<{index_count}H", indices, 4)
    bone = [struct.unpack_from("<I", vertices, 8 + i * VERTEX_SIZE + 56)[0]
            for i in range(vertex_count)]
    parent = list(range(vertex_count))

    def find(index):
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    triangles = [raw_indices[i:i + 3] for i in range(0, index_count, 3)]
    for triangle in triangles:
        kinds = {bone[index] for index in triangle}
        if len(kinds) != 1:
            raise ValueError("mixed-bone triangle requires a different extraction path")
        if ROUND_BONE in kinds:
            a, b, c = triangle
            parent[find(b)] = find(a)
            parent[find(c)] = find(a)

    round_parts = defaultdict(list)
    for index, kind in enumerate(bone):
        if kind == ROUND_BONE:
            round_parts[find(index)].append(index)
    centers = []
    for root, component in round_parts.items():
        ys = [struct.unpack_from("<f", vertices, 8 + index * VERTEX_SIZE + 4)[0]
              for index in component]
        centers.append((sum(ys) / len(ys), root))
    low, high = min(y for y, _ in centers), max(y for y, _ in centers)
    pitch = (high - low) / 7
    if not 0.008 < pitch < 0.011:
        raise ValueError("stock PM round spacing changed")
    round_layer = {root: round((y - low) / pitch) for y, root in centers}
    if set(round_layer.values()) != set(range(8)):
        raise ValueError("expected exactly eight separated PM cartridge layers")

    output.mkdir(parents=True, exist_ok=True)
    for count in range(9):
        keep = []
        for triangle in triangles:
            kind = bone[triangle[0]]
            if kind in MAGAZINE_BONES or (kind == ROUND_BONE and
                    round_layer[find(triangle[0])] >= 8 - count):
                keep.extend(triangle)
        used = sorted(set(keep))
        remap = {old: new for new, old in enumerate(used)}
        new_vertices = (struct.pack("<II", fmt, len(used)) +
                        b"".join(vertices[8 + i * VERTEX_SIZE:8 + (i + 1) * VERTEX_SIZE]
                                 for i in used))
        new_indices = (struct.pack("<I", len(keep)) +
                       struct.pack(f"<{len(keep)}H", *(remap[i] for i in keep)))
        new_main = pack((kind, new_vertices if kind == 3 else
                         new_indices if kind == 4 else payload)
                        for kind, payload in main)
        new_children = pack([(0, new_main)])
        result = pack((kind, new_children if kind == 9 else payload)
                      for kind, payload in top)
        path = output / f"wpn_pm_mag_{count}.ogf"
        path.write_bytes(result)
        print(f"{path}: {len(used)} vertices, {len(keep)//3} triangles")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--output", type=Path, default=DEST)
    args = parser.parse_args()
    generate(args.source, args.output)
