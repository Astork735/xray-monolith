"""Check enabled stock VR profiles against audited installed OGF geometry."""
import configparser
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
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
def chunks(data):
    offset = 0
    while offset + 8 <= len(data):
        kind, size = struct.unpack_from('<II', data, offset)
        assert offset + 8 + size <= len(data)
        yield kind, data[offset + 8:offset + 8 + size]
        offset += 8 + size
    assert offset == len(data)


pm = profiles['vr_wpn_wpn_pm']
assert pm['magazine_visual_prefix'] == r'vr\wpn_pm_mag_'
assert pm.getint('magazine_visual_max_rounds') == 8
pm_bones = {bone['name']: bone for bone in report['weapons']['pm']['model_candidates'][0]['bones']}
pm_base = [float(value) for value in pm['mag_base'].split(',')]
pm_grip = [float(value) for value in pm['mag_grip'].split(',')]
assert abs(pm_base[1] - pm_bones['magazin']['mesh_min'][1]) < 0.003
assert all(abs(a + b) < 0.002 for a, b in zip(pm_base, pm_grip))
assert 0.09 <= pm.getfloat('extraction') <= 0.11
for count in range(9):
    path = ROOT / f'gamedata/meshes/vr/wpn_pm_mag_{count}.ogf'
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
print('PASS: audited stock PM, AK-74 and USP-45 VR profiles')
