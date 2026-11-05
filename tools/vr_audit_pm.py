"""Read installed PM asset metadata; never changes or extracts game files.
Run from an x64 VS developer prompt. --vcvars can initialize that environment.
"""
import argparse, collections, ctypes as c, hashlib, json, mmap, struct, subprocess, zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MODEL=r'meshes\anomaly_weapons\wpn_pm\wpn_pm_hud.ogf'
CONFIG=r'configs\items\weapons\w_pm.ltx'
def chunks(data):
    offset=0
    while offset+8<=len(data):
        kind,size=struct.unpack_from('<II',data,offset)
        if offset+8+size>len(data): raise ValueError('Chunk exceeds file')
        yield kind,offset+8,size
        offset+=8+size

def load_decoder(path):
    dll=c.CDLL(str(path))
    dll.unpack.argtypes=[c.POINTER(c.c_void_p),c.POINTER(c.c_uint),c.c_void_p,c.c_uint]
    dll.release.argtypes=[c.c_void_p]
    dll.unlzo.argtypes=[c.c_void_p,c.c_ulonglong,c.c_void_p,c.POINTER(c.c_ulonglong)]
    return dll

def build_decoder(vcvars):
    folder=ROOT/'_build/vr-asset-decoder';folder.mkdir(parents=True,exist_ok=True)
    # Reuse a decoder built from the current engine sources when no compiler
    # environment is given (cl is often not on PATH).
    built=folder/'decoder.dll'
    sources=[ROOT/'src/xrCore/LzHuf.cpp',ROOT/'src/xrCore/rt_lzo1x_d1.cpp']
    if not vcvars and built.exists() and all(built.stat().st_mtime>=s.stat().st_mtime for s in sources):
        return load_decoder(built)
    s=(ROOT/'src/xrCore/LzHuf.cpp').read_text().replace('#include "stdafx.h"', '''#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
using u8=unsigned char; using u32=uint32_t;
#define IC inline
#define xr_malloc malloc
#define xr_realloc realloc
#define xr_free free''')
    s+='\nextern "C" __declspec(dllexport) void unpack(u8** d,unsigned* n,void* s,unsigned z){_decompressLZ(d,n,s,z);}\nextern "C" __declspec(dllexport) void release(void* p){free(p);}\n'
    (folder/'decoder.cpp').write_text(s)
    s=(ROOT/'src/xrCore/rt_lzo1x_d1.cpp').read_text().replace('#include "stdafx.h"','')
    s+='\nextern "C" __declspec(dllexport) int unlzo(const unsigned char* s,unsigned long long n,unsigned char* d,unsigned long long* z){lzo_uint out=(lzo_uint)*z;int r=lzo1x_decompress(s,(lzo_uint)n,d,&out,0);*z=out;return r;}\n'
    (folder/'lzo.cpp').write_text(s)
    lines=[f'@cd /d "{folder}"']
    if vcvars:lines.append(f'@call "{vcvars}" >nul')
    lines.append(f'cl /nologo /LD /O2 decoder.cpp lzo.cpp /I"{ROOT / "src/xrCore"}" /link /OUT:decoder.dll')
    batch=folder/'build.cmd';batch.write_text('\n'.join(lines)+'\n')
    result=subprocess.run(['cmd.exe','/d','/c',str(batch)],capture_output=True)
    if result.returncode:raise RuntimeError(result.stdout.decode(errors='replace')+result.stderr.decode(errors='replace'))
    dll=c.CDLL(str(folder/'decoder.dll'))
    dll.unpack.argtypes=[c.POINTER(c.c_void_p),c.POINTER(c.c_uint),c.c_void_p,c.c_uint]
    dll.release.argtypes=[c.c_void_p]
    dll.unlzo.argtypes=[c.c_void_p,c.c_ulonglong,c.c_void_p,c.POINTER(c.c_ulonglong)]
    return dll

class Archive:
    def __init__(self,path,dll):
        self.path,self.dll,self.entries=path,dll,{}
        with path.open('rb') as file,mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
            for kind,offset,size in chunks(data):
                if kind!=0x80000001:continue
                pointer,count=c.c_void_p(),c.c_uint()
                dll.unpack(c.byref(pointer),c.byref(count),data[offset:offset+size],size)
                table=c.string_at(pointer,count.value);dll.release(pointer);cursor=0
                while cursor<len(table):
                    size=struct.unpack_from('<H',table,cursor)[0];cursor+=2
                    real,compressed,crc=struct.unpack_from('<III',table,cursor)
                    name=table[cursor+12:cursor+size-4].decode('cp1251')
                    offset=struct.unpack_from('<I',table,cursor+size-4)[0];cursor+=size
                    self.entries[name]=(real,compressed,crc,offset)
    def read(self,name):
        real,compressed,crc,offset=self.entries[name]
        with self.path.open('rb') as file:file.seek(offset);data=file.read(compressed)
        if real!=compressed:
            output,count=c.create_string_buffer(real),c.c_ulonglong(real)
            result=self.dll.unlzo(data,compressed,output,c.byref(count))
            if result or count.value!=real:raise ValueError('LZO decode failed')
            data=output.raw
        if zlib.crc32(data)!=crc:raise ValueError('CRC mismatch')
        return data

def audit(data):
    top={k:data[p:p+n] for k,p,n in chunks(data)}
    table=top[13];cursor=4;bones=[]
    for index in range(struct.unpack_from('<I',table)[0]):
        end=table.index(0,cursor);name=table[cursor:end].decode();cursor=end+1
        end=table.index(0,cursor);parent=table[cursor:end].decode();cursor=end+61
        bones.append(dict(index=index,name=name,parent=parent))
    ik=top[16];cursor=0
    for bone in bones:
        version=struct.unpack_from('<I',ik,cursor)[0];cursor=ik.index(0,cursor+4)+1+112+72+(4 if version else 0)
        bone['bind_rotation']=struct.unpack_from('<3f',ik,cursor)
        bone['bind_position']=struct.unpack_from('<3f',ik,cursor+12);cursor+=40
    if cursor!=len(ik):raise ValueError('Unexpected IK layout')
    positions=collections.defaultdict(list)
    for _,offset,size in chunks(top[9]):
        child=top[9][offset:offset+size]
        vertices=next(child[p:p+n] for k,p,n in chunks(child) if k==3)
        fmt,count=struct.unpack_from('<II',vertices)
        if fmt!=0x12071980:raise ValueError('Unexpected vertex format')
        for i in range(count):
            cursor=8+i*60;bone=struct.unpack_from('<I',vertices,cursor+56)[0]
            positions[bone].append(struct.unpack_from('<3f',vertices,cursor))
    for bone in bones:
        vertices=positions[bone['index']];bone['vertex_count']=len(vertices)
        if vertices:
            bone['mesh_min']=[min(v[i] for v in vertices) for i in range(3)]
            bone['mesh_max']=[max(v[i] for v in vertices) for i in range(3)]
    return bones

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--vcvars',type=Path)
    args=parser.parse_args();dll=build_decoder(args.vcvars)
    paths=[args.game/'db/meshes/meshes_weapons.db0',args.game/'db/configs/configs.db0']
    for folder in ['patches','mods','addons']:paths.extend(sorted((args.game/'db'/folder).rglob('*.db*')))
    candidates={MODEL:[],CONFIG:[]};models=[]
    for path in paths:
        if not path.is_file():continue
        archive=Archive(path,dll)
        for name in candidates:
            if name not in archive.entries:continue
            data=archive.read(name);entry=dict(archive=str(path),sha256=hashlib.sha256(data).hexdigest(),bytes=len(data))
            candidates[name].append(entry)
            if name==MODEL:models.append(dict(source=str(path),bones=audit(data)))
    for name in candidates:
        loose=args.game/'gamedata'/Path(*name.split('\\')[1:])
        if loose.is_file():
            data=loose.read_bytes();candidates[name].append(dict(loose=str(loose),sha256=hashlib.sha256(data).hexdigest(),bytes=len(data)))
    report=dict(game=str(args.game),candidates=candidates,models=models,notes=[
        'Reports all candidates; does not assume archive override precedence.',
        'Grip offset and slide travel are calibration values, not named anchors.',
        'Geometry audit does not validate HMD grip alignment or physical reach.'])
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(f'Audited {len(models)} PM model candidate(s): {args.output}')
if __name__=='__main__':main()
