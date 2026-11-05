"""Check enabled stock VR profiles against audited installed OGF geometry."""
import configparser
import json
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from vr_extract_pm_magazines import chunks  # noqa: E402
profiles = configparser.ConfigParser(interpolation=None)
profiles.read(ROOT / 'gamedata/configs/vr/vr_weapons.ltx', encoding='utf-8')
report = json.loads((ROOT / 'docs/vr-weapon-assets.json').read_text(encoding='utf-8'))

for name, section in (('pm', 'vr_wpn_wpn_pm'),
                      ('ak74', 'vr_wpn_wpn_ak74'), ('usp45', 'vr_wpn_wpn_usp')):
    profile = profiles[section]
    candidates = report['weapons'][name]['model_candidates']
    assert len(candidates) == 1, f'{name}: ambiguous installed model'
    bones = {bone['name']: bone for bone in candidates[0]['bones']}
    visual = profile['expected_visual'].replace('/', '\\').lower()
    assert candidates[0]['source'].lower().endswith('meshes_weapons.db0')
    assert visual.endswith(f'wpn_{name}_hud.ogf') if name != 'usp45' else visual.endswith('wpn_usp45_hud.ogf')
    for label, expected in (('mag_bone', 'expected_mag_bind'),
                            ('cycle_bone', 'expected_cycle_bind')):
        bone = bones[profile[label]]
        position = [float(x.strip()) for x in profile[expected].split(',')]
        assert max(abs(a-b) for a, b in zip(bone['bind_position'], position)) < 0.005, (name, label)
    assert profile['rounds_bone'] in bones
    for key in ('mag_extra_bone_1', 'mag_extra_bone_2'):
        if key in profile:
            assert profile[key] in bones
    mag = bones[profile['mag_bone']]
    center = [float(x.strip()) for x in profile['mag_box_center'].split(',')]
    half = [float(x.strip()) for x in profile['mag_box_half'].split(',')]
    assert all(a-h-0.001 <= lo and hi <= a+h+0.001
               for a, h, lo, hi in zip(center, half, mag['mesh_min'], mag['mesh_max'])), name
    assert profile.getboolean('detachable_magazine')
    assert profile.getboolean('chamber')
assert not profiles['vr_wpn_wpn_ak74'].getboolean('slide_stop')

# Hand grips, triggers, magazine wells and shotgun mechanisms are measured
# from each weapon's own mesh at runtime; profiles carry no hand poses.
for section in profiles.sections():
    for key in ('hand_grip_center', 'hand_grip_angle', 'hand_grip_radius', 'trigger_point'):
        assert key not in profiles[section], (section, key)
assert profiles['vr_wpn_wpn_usp'].getboolean('slide_stop')

# A dedicated PM mesh must never contain pistol-body or slide triangles. The
# eight original cartridge layers are individually retained by round count.
pm = profiles['vr_wpn_wpn_pm']
assert pm['magazine_visual_prefix'] == r'vr\wpn_pm_mag_'
assert pm.getint('magazine_visual_max_rounds') == 8
pm_bones = {bone['name']: bone for bone in report['weapons']['pm']['model_candidates'][0]['bones']}
pm_base = [float(value) for value in pm['mag_base'].split(',')]
pm_grip = [float(value) for value in pm['mag_grip'].split(',')]
assert abs(pm_base[1] - pm_bones['magazin']['mesh_min'][1]) < 0.003
assert all(abs(a + b) < 0.002 for a, b in zip(pm_base, pm_grip))
assert 0.09 <= pm.getfloat('extraction') <= 0.11
print('PASS: audited stock PM, AK-74 and USP-45 VR profiles')

# The meshes are not in the repository: they are cut from the installed game's
# models by tools/vr_extract_pm_magazines.py and tools/vr_extract_sks_round.py
# into gamedata/meshes/vr (or VR_MESHES). Without them their checks are skipped.
MESHES = Path(os.environ.get('VR_MESHES', ROOT / 'gamedata/meshes/vr'))
pm_meshes = [MESHES / f'wpn_pm_mag_{count}.ogf' for count in range(9)]
if all(path.exists() for path in pm_meshes):
    for count, path in enumerate(pm_meshes):
        top = dict(chunks(path.read_bytes()))
        children = list(chunks(top[9]))
        assert len(children) == 1
        mesh = dict(chunks(children[0][1]))
        fmt, vertex_count = struct.unpack_from('<II', mesh[3])
        assert fmt == 0x12071980 and len(mesh[3]) == 8 + vertex_count * 60
        bone_ids = [struct.unpack_from('<I', mesh[3], 8 + i * 60 + 56)[0]
                    for i in range(vertex_count)]
        assert bone_ids.count(0) == bone_ids.count(7) == 0
        assert bone_ids.count(4) == 322
        assert bone_ids.count(6) == 164
        assert bone_ids.count(8) == 1698
        assert bone_ids.count(1) == 232 * count
        index_count = struct.unpack_from('<I', mesh[4])[0]
        assert len(mesh[4]) == 4 + index_count * 2 and index_count % 3 == 0
        indices = struct.unpack_from(f'<{index_count}H', mesh[4], 4)
        assert all(index < vertex_count for index in indices)
        assert all(len({bone_ids[index] for index in indices[i:i + 3]}) == 1
                   for i in range(0, index_count, 3))
    print('PASS: PM magazine meshes keep only the magazine and its rounds')
else:
    print(f'SKIP: PM magazine meshes not extracted in {MESHES}')

# The SKS pouch asset contains one live cartridge, with no clip, case-only
# substitute, weapon triangles or dependency on the weapon's animation set.
# Its 56 mm length is loose_rounds' 0.028 m half length in
# gamedata/configs/vr/vr_weapon_mechanics.ltx.
round_path = MESHES / 'round_762x39.ogf'
if not round_path.exists():
    print(f'SKIP: SKS round mesh not extracted in {MESHES}')
    sys.exit(0)
round_data = dict(chunks(round_path.read_bytes()))
assert set(round_data) == {1, 9, 13, 16}
assert round_data[1][:2] == bytes((4, 10))  # rigid skeletal visual
round_children = list(chunks(round_data[9]))
assert len(round_children) == 1
round_mesh = dict(chunks(round_children[0][1]))
assert round_mesh[2].split(b'\0')[0] == rb'wpn\wpn_sks\wpn_sks_bullet'
fmt, count = struct.unpack_from('<II', round_mesh[3])
assert fmt == 1 and count == 70
points = [struct.unpack_from('<3f', round_mesh[3], 8 + i * 60) for i in range(count)]
assert all(struct.unpack_from('<I', round_mesh[3], 8 + i * 60 + 56)[0] == 9 for i in range(count))
low = [min(p[c] for p in points) for c in range(3)]
high = [max(p[c] for p in points) for c in range(3)]
assert all(abs(a + b) < 1e-6 for a, b in zip(low, high))
assert abs(high[2] - low[2] - 0.056) < 1e-6
assert all(0.009 < high[c] - low[c] < 0.012 for c in (0, 1))
index_count = struct.unpack_from('<I', round_mesh[4])[0]
assert index_count == 78 * 3
indices = struct.unpack_from(f'<{index_count}H', round_mesh[4], 4)
assert set(indices) == set(range(count))
print('PASS: one complete, centred SKS 7.62x39 cartridge')
