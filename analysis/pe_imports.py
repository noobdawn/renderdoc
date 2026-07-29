# -*- coding: utf-8 -*-
"""Minimal PE parser: dump import + delay-import directories of Game.exe"""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else r"E:\renderdoc_mod\analysis\Game.exe"
data = open(path, "rb").read()
fullsize = len(data)
print("file size:", fullsize)

def u16(o): return struct.unpack_from("<H", data, o)[0]
def u32(o): return struct.unpack_from("<I", data, o)[0]
def u64(o): return struct.unpack_from("<Q", data, o)[0]

e_lfanew = u32(0x3C)
print("MZ magic:", data[:2], "e_lfanew=0x%x" % e_lfanew)
if data[e_lfanew:e_lfanew+4] != b"PE\0\0":
    print("PE signature MISSING -> on-disk file is packed/protected")
    sys.exit(0)

filehdr = e_lfanew + 4
num_sections = u16(filehdr + 2)
opt = filehdr + 20
magic = u16(opt)
is64 = (magic == 0x20B)
print("OptionalHeader magic=0x%x (%s), sections=%d" % (magic, "PE32+" if is64 else "PE32", num_sections))

dd_base = opt + (112 if is64 else 96)
dirs = []
for i in range(16):
    rva = u32(dd_base + i*8); sz = u32(dd_base + i*8 + 4)
    dirs.append((rva, sz))

NAMES = {0:"EXPORT",1:"IMPORT",2:"RESOURCE",3:"EXCEPTION",4:"SECURITY",5:"BASERELOC",
         6:"DEBUG",7:"ARCH",8:"GLOBALPTR",9:"TLS",10:"LOAD_CONFIG",11:"BOUND_IMPORT",
         12:"IAT",13:"DELAY_IMPORT",14:"COM_DESCRIPTOR",15:"RESERVED"}
for i,(rva,sz) in enumerate(dirs):
    if rva: print("DataDir[%2d] %-12s RVA=0x%08x Size=0x%x" % (i, NAMES.get(i,"?"), rva, sz))

# section table for RVA->file offset mapping
sec = opt + (240 if is64 else 224)
sections = []
for i in range(num_sections):
    o = sec + i*40
    name = data[o:o+8].rstrip(b"\0").decode("ascii", "replace")
    vsize = u32(o+8); vaddr = u32(o+12); rawsize = u32(o+16); rawptr = u32(o+20)
    sections.append((vaddr, vsize, rawptr, rawsize, name))

def rva2off(rva):
    for vaddr, vsize, rawptr, rawsize, name in sections:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None

print("\nSections:")
for vaddr, vsize, rawptr, rawsize, name in sections:
    print("  %-8s VA=0x%08x VSize=0x%08x Raw=0x%08x" % (name, vaddr, vsize, rawsize))

def cstr(off):
    end = data.find(b"\0", off)
    return data[off:end].decode("ascii", "replace")

def dump_imports(dir_index, title, is64):
    rva, sz = dirs[dir_index]
    print("\n=== %s (RVA=0x%x size=0x%x) ===" % (title, rva, sz))
    if rva == 0:
        print("  <EMPTY>")
        return
    off = rva2off(rva)
    if off is None:
        print("  RVA maps outside sections (raw data in header?) off=None")
        off = rva  # try direct
    idx = 0
    while True:
        d = off + idx*20
        if d + 20 > len(data):
            print("  ... descriptor exceeds read window")
            break
        oft = u32(d); ft = u32(d+16); nameRVA = u32(d+12)
        if oft == 0 and ft == 0 and nameRVA == 0:
            break
        noff = rva2off(nameRVA)
        dllname = cstr(noff) if noff else "<bad name rva 0x%x>" % nameRVA
        funcs = []
        # read thunks from ILT if present, else from IAT
        thunkRVA = oft if oft else ft
        toff = rva2off(thunkRVA)
        if toff is not None:
            step = 8 if is64 else 4
            for j in range(3000):
                t = toff + j*step
                if t + step > len(data): break
                val = u64(t) if is64 else u32(t)
                if val == 0: break
                if is64 and (val & (1<<63)):
                    funcs.append("#%d" % (val & 0xFFFF)); continue
                if not is64 and (val & (1<<31)):
                    funcs.append("#%d" % (val & 0xFFFF)); continue
                foff = rva2off(val & 0x7FFFFFFF)
                if foff is None or foff+3 > len(data): break
                funcs.append(cstr(foff+2))
        idx += 1
        interesting = [f for f in funcs if any(k in f.lower() for k in
            ("loadlibrary", "getprocaddress", "d3d", "dxgi", "createprocess", "virtualprotect",
             "freelibrary", "getmodulehandle", "ldr"))]
        print("  [%2d] %-28s ILT=0x%08x IAT=0x%08x funcs=%d" % (idx-1, dllname, oft, ft, len(funcs)))
        if interesting:
            print("       -> %s" % ", ".join(interesting[:12]))

dump_imports(1, "REGULAR IMPORTS", is64)
dump_imports(13, "DELAY IMPORTS", is64)
