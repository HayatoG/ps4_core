import struct, sys, re, collections, json
# uso: sce_imports.py <eboot.elf> <nid_db.xml> <saida.json>  — lista importações SCE por biblioteca, resolvendo NIDs
path, nidxml, out = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(path,'rb').read()
phoff = struct.unpack_from('<Q', d, 0x20)[0]; phnum = struct.unpack_from('<H', d, 0x38)[0]
ph = [struct.unpack_from('<IIQQQQQQ', d, phoff+i*56) for i in range(phnum)]
dyn = next(p for p in ph if p[0]==2); dld = next(p for p in ph if p[0]==0x61000000)
base = dld[2]
tags = collections.defaultdict(list)
for off in range(dyn[2], dyn[2]+dyn[5], 16):
    t,v = struct.unpack_from('<qQ', d, off)
    if t==0: break
    tags[t & 0xffffffffffffffff].append(v)
strtab = base + tags[0x61000035][0]
def s(o):
    e = d.index(b'\0', strtab+o); return d[strtab+o:e].decode()
mods = {}
for v in tags[0x6100000F]: mods[v>>48] = s(v & 0xffffffff)
libs = {}
for v in tags[0x61000015]: libs[v>>48] = s(v & 0xffffffff)
nid = dict(re.findall(r'obf="([^"]+)" sym="([^"]*)"', open(nidxml).read()))
symtab = base + tags[0x61000039][0]; symsz = tags[0x6100003F][0]
b64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
def dec(x):
    n=0
    for c in x: n = n*64 + b64.index(c)
    return n
per = collections.defaultdict(list)
for off in range(symtab, symtab+symsz, 24):
    name, info, other, shndx, val, size = struct.unpack_from('<IBBHQQ', d, off)
    if shndx != 0 or name == 0: continue
    n = s(name); parts = n.split('#')
    if len(parts) != 3: continue
    lib = libs.get(dec(parts[1]), parts[1])
    per[lib].append(nid.get(parts[0], parts[0]))
print("NEEDED MODULES:", sorted(mods.values()))
res = {}
for lib in sorted(per, key=lambda k:-len(per[k])):
    syms = sorted(set(per[lib])); res[lib] = syms
    known = sum(1 for x in syms if not re.fullmatch(r'[A-Za-z0-9+-]{11}', x))
    print(f"{lib:32s} {len(syms):5d} imports ({known} resolved)")
json.dump({'modules':mods,'libs':res}, open(out,'w'), indent=1)
