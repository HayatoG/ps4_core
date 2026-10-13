# uso: OUT=<dir> psarc_toc.py <arquivo.psarc>...  — lê header/TOC de PSARC e grava o manifest (lista de paths)
import sys, struct, zlib, lzma, collections, os
def parse(path, dump_manifest=None):
    f=open(path,'rb'); h=f.read(32)
    magic,vmaj,vmin,comp,toclen,esz,cnt,bs,flags=struct.unpack('>4sHH4sIIIII',h)
    print(f'== {path}  size={os.path.getsize(path)/1e9:.2f}GB')
    print(f' magic={magic} ver={vmaj}.{vmin} comp={comp} toc_len={toclen} entry_size={esz} entries={cnt} block={bs} flags={flags:#x} (1=ignorecase,2=absolute)')
    toc=f.read(toclen-32)
    ents=[]
    for i in range(cnt):
        e=toc[i*esz:(i+1)*esz]
        md5=e[:16]; zi=struct.unpack('>I',e[16:20])[0]
        usz=int.from_bytes(e[20:25],'big'); off=int.from_bytes(e[25:30],'big')
        ents.append((md5,zi,usz,off))
    # block size table
    bt=toc[cnt*esz:]
    nb=1; w=2
    while (1<<(8*w))<bs: w+=1
    zs=[int.from_bytes(bt[i:i+w],'big') for i in range(0,len(bt)-len(bt)%w,w)]
    print(f' zsize width={w} blocks={len(zs)}')
    def read_entry(idx):
        md5,zi,usz,off=ents[idx]; out=b''; f.seek(off); b=zi
        while len(out)<usz:
            z=zs[b]; raw=f.read(z if z else bs)
            if z==0 or len(raw)==min(bs,usz-len(out)) and not raw.startswith(b'\x78') and not raw.startswith(b'\x5d'):
                out+=raw
            else:
                try: out+= zlib.decompress(raw) if raw[:1]==b'\x78' else lzma.decompress(raw)
                except Exception: out+=raw
            b+=1
        return out[:usz]
    man=read_entry(0).decode('utf8','replace')
    import re
    names=[n for n in re.split('[\n\x00]',man) if n]
    print(f' manifest names={len(names)} (entries-1={cnt-1})')
    import hashlib
    idx={e[0]:i for i,e in enumerate(ents)}
    order=[idx.get(hashlib.md5(n.upper().encode()).digest(), None) for n in names]
    print(f' md5(upper(path)) matched: {sum(o is not None for o in order)}; manifest order==toc order: {order==list(range(1,cnt))}')
    ents_by_name=[ents[o] if o is not None else (b'',0,0,0) for o in order]
    if dump_manifest: open(dump_manifest,'w').write('\n'.join(names))
    ext=collections.Counter(); top=collections.Counter(); top2=collections.Counter(); sz=collections.Counter()
    for n,e in zip(names,ents_by_name):
        p=n.lstrip('/'); parts=p.split('/')
        x=os.path.splitext(p)[1].lower() or '(none)'; ext[x]+=1; sz[x]+=e[2]
        top[parts[0]]+=1; top2['/'.join(parts[:2])]+=1
    print(' top dirs:', top.most_common(15))
    print(' 2-level:', top2.most_common(30))
    print(' ext (count, uncompressed MB):', [(k,v,round(sz[k]/1e6,1)) for k,v in ext.most_common(40)])
    print(' sample:', names[:8], names[len(names)//2:len(names)//2+5])
    fsz=os.path.getsize(path); missing=sum(1 for e in ents[1:] if e[3]>=fsz); missb=sum(e[2] for e in ents[1:] if e[3]>=fsz)
    print(f' entries with data offset beyond EOF: {missing}/{cnt-1} ({missb/1e9:.2f} GB uncompressed missing)')
    # peek at first .lua and a few magic bytes per ext
    seen={}
    for i,(n,e) in enumerate(zip(names,ents_by_name)):
        x=os.path.splitext(n)[1].lower()
        if x not in seen and e[3]<fsz and e[2]>0:
            try: seen[x]=(n,read_entry(order[i])[:16])
            except Exception as ex: seen[x]=(n,str(ex))
    for k,v in seen.items(): print('  magic',k,v)
    return names
for p in sys.argv[1:]:
    parse(p, os.path.join(os.environ.get('OUT', '.'), os.path.basename(p)+'.manifest.txt'))
