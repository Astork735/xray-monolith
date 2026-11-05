"""Audit installed HUD weapons and configuration without extracting game files.

Requires an x64 MSVC toolchain to build the existing archive decoder. The
report identifies the effective source only when exactly one candidate exists;
ambiguous archive override order is deliberately reported as unresolved.
"""
import argparse
import hashlib
import json
from pathlib import Path

from vr_audit_pm import Archive, audit, build_decoder


def audit_any(data):
    """Bones of a HUD model. Single-link meshes give the exact audit; others
    (multi-link skinning, as the PPSh-41 and PKM use) take each vertex's
    strongest bone through the runtime measurement reader."""
    try:
        return audit(data)
    except ValueError:
        from vr_measure_weapons import load
        bones = []
        for index, bone in enumerate(load(data)):
            entry = dict(index=index, name=bone['name'], parent=bone['parent'],
                         bind_rotation=None, bind_position=list(bone['pos']),
                         vertex_count=len(bone['pts']))
            if bone['pts']:
                entry['mesh_min'] = [min(v[i] for v in bone['pts']) for i in range(3)]
                entry['mesh_max'] = [max(v[i] for v in bone['pts']) for i in range(3)]
            bones.append(entry)
        return bones

WEAPONS = {
    'pm': (r'meshes\anomaly_weapons\wpn_pm\wpn_pm_hud.ogf',
           r'configs\items\weapons\w_pm.ltx'),
    'ak74': (r'meshes\anomaly_weapons\wpn_ak74\wpn_ak74_hud.ogf',
             r'configs\items\weapons\w_ak74.ltx'),
    'usp45': (r'meshes\anomaly_weapons\wpn_usp45\wpn_usp45_hud.ogf',
              r'configs\items\weapons\w_usp45.ltx'),
    'ppsh41': (r'meshes\anomaly_weapons\wpn_ppsh41\wpn_ppsh41_hud.ogf',
               r'configs\items\weapons\w_ppsh41.ltx'),
    'pkm': (r'meshes\anomaly_weapons\wpn_pkm\wpn_pkm_hud.ogf',
            r'configs\items\weapons\w_pkm.ltx'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--vcvars', type=Path)
    args = parser.parse_args()
    decoder = build_decoder(args.vcvars)
    paths = sorted((args.game / 'db/meshes').glob('*.db*'))
    paths += sorted((args.game / 'db/configs').glob('*.db*'))
    for folder in ('patches', 'mods', 'addons'):
        root = args.game / 'db' / folder
        if root.exists():
            paths.extend(sorted(root.rglob('*.db*')))
    keys = {key for pair in WEAPONS.values() for key in pair}
    candidates = {key: [] for key in keys}
    for path in paths:
        if not path.is_file():
            continue
        archive = Archive(path, decoder)
        for key in keys & archive.entries.keys():
            data = archive.read(key)
            entry = {'source': str(path), 'sha256': hashlib.sha256(data).hexdigest()}
            if key.endswith('.ogf'):
                entry['bones'] = audit_any(data)
            else:
                lines = data.decode('cp1251').splitlines()
                entry['weapon_settings'] = [line.strip() for line in lines
                    if any(term in line.lower() for term in
                           ('item_visual', 'ammo_class', 'ammo_mag_size',
                            'fire_bone', 'fire_point', 'shell_bone', 'shell_point',
                            'anm_reload'))]
            candidates[key].append(entry)
    for key in keys:
        loose = args.game / 'gamedata' / Path(*key.split('\\'))
        if loose.is_file():
            data = loose.read_bytes()
            candidates[key].append({'source': str(loose),
                'sha256': hashlib.sha256(data).hexdigest(),
                'bones': audit_any(data) if key.endswith('.ogf') else None})
    report = {'game': str(args.game), 'weapons': {}}
    for name, (model, config) in WEAPONS.items():
        report['weapons'][name] = {
            'model_candidates': candidates[model],
            'config_candidates': candidates[config],
            'effective_model': candidates[model][0]['source']
                if len(candidates[model]) == 1 else 'unresolved',
            'effective_config': candidates[config][0]['source']
                if len(candidates[config]) == 1 else 'unresolved',
        }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'Wrote {args.output}')


if __name__ == '__main__':
    main()
