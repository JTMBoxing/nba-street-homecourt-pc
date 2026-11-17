# makepatches.py <port folder> <game.iso> <xdelta3> <out dir> [seconds]
# Builds the beta's patch set: every file we changed or made FROM one of the game's own files becomes an xdelta3 (VCDIFF) patch
# that only works on the tester's own disc data. Writes <out>/patches/NNNN.vcdiff + <out>/patches.tsv:
#   target  kind  source  entry  source_md5  target_md5  target_size  patch_size
#   kind = disc (a file on the disc) | big (a file inside one of the disc's .big archives) | new (our own file: shipped as is, no patch)
# Resumable (targets already in patches.tsv are skipped) and stops after [seconds].
import sys,os,struct,hashlib,subprocess,time,re
P,ISO,XD,OUT=sys.argv[1:5]; budget=float(sys.argv[5]) if len(sys.argv)>5 else 160; t0=time.time()
sys.path.insert(0,os.path.join(P,'dev','tools')); from meshlib import ents
os.makedirs(OUT+'/patches',exist_ok=True); os.makedirs(OUT+'/tmp',exist_ok=True)
md5=lambda b: hashlib.md5(b).hexdigest()
# ---- the disc
f=open(ISO,'rb'); SEC=2048; base=None
for off in (0x0,0xFD90000,0x2080000,0x18300000):
    f.seek(off+32*SEC)
    if f.read(0x14)==b'MICROSOFT*XBOX*MEDIA': base=off; break
f.seek(base+32*SEC+0x14); rs,rz=struct.unpack('<II',f.read(8)); DISC={}
def walk(sec,size,prefix):
    f.seek(base+sec*SEC); d=f.read(size)
    def node(o):
        if o*4+14>len(d): return
        l,r,fs,fz,attr,nl=struct.unpack_from('<HHIIBB',d,o*4)
        if l==0xFFFF: return
        name=d[o*4+14:o*4+14+nl].decode('latin-1')
        if l: node(l)
        if attr&0x10:
            if fz: walk(fs,fz,prefix+name+'/')
        else: DISC[(prefix+name).lower()]=(prefix+name,fs,fz)
        if r: node(r)
    node(0)
walk(rs,rz,'/')
def disc_bytes(p):
    n,sec,sz=DISC[p.lower()]; f.seek(base+sec*SEC); return f.read(sz)
# ---- archive entries (from the DISC copy of each .big, read in place inside the iso)
BIGS={}
def big_index(dp):
    if dp in BIGS: return BIGS[dp]
    tmp=OUT+'/tmp/idx.big'; n,sec,sz=DISC[dp.lower()]
    f.seek(base+sec*SEC); hdr=f.read(min(sz,32<<20)); open(tmp,'wb').write(hdr)     # index sits at the start of a BIG file
    try: fh,es=ents(tmp); fh.close(); idx={e[0].rsplit('/',1)[-1].lower():(e[0],e[1],e[2]) for e in es}
    except Exception as ex: print('index fail',dp,ex); idx={}
    BIGS[dp]=(sec,idx); return BIGS[dp]
def big_bytes(dp,entry):
    sec,idx=big_index(dp); name,off,size=idx[entry.lower()]; f.seek(base+sec*SEC+off); return f.read(size),name
# ---- what to patch
G=os.path.join(P,'game'); targets=[]
man=[l.rstrip('\n').split('\t') for l in open(os.path.join(P,'_tmp/release/manifest.tsv'),encoding='utf-8')]
for p,sz,a,b,st in man:
    if st=='CHANGED' and b!='MISSING': targets.append(('/'+p.lstrip('/'),'disc',p,''))
SUFFIX=re.compile(r'_(mod|drive|drive_noflip|stock|noflip|custom|rstick|lebronshot_v1)(?=\.)',re.I)
for root,_,fs in os.walk(os.path.join(G,'data')):
    for fn in fs:
        rel='/'+os.path.relpath(os.path.join(root,fn),G).replace('\\','/')
        if rel.lower() in DISC or re.search(r'\.(before|orig|sleeves)',fn) or '/audio/custom/' in rel: continue
        src=None
        m=SUFFIX.search(fn)
        if m:
            cand=os.path.dirname(rel)+'/'+fn[:m.start()]+fn[m.end():]
            for c in (cand, os.path.dirname(rel)+'/'+fn[:m.start()]+'.big', os.path.dirname(rel)+'/'+fn[:m.start()]+'s'+fn[m.end():]):
                if c.lower() in DISC: src=c; break
        targets.append((rel,'disc' if src else 'new',src or '',''))
for l in open(os.path.join(P,'_tmp/release/modclass.tsv'),encoding='utf-8'):
    p,sz,k=l.rstrip('\n').split('\t'); rel='/'+p.replace('\\','/')[len('game/'):]
    if '/mods/hd' in rel: continue                                   # HD textures: built on the tester's PC later, not shipped
    if k.startswith('EDIT-OF'):
        arch='/'+k.split(' ',1)[1].replace('\\','/')[len('game/'):]
        targets.append((rel,'big',arch,os.path.basename(p)))
    else: targets.append((rel,'new','',''))
done=set()
TSV=OUT+'/patches.tsv'
if os.path.exists(TSV): done={l.split('\t')[0] for l in open(TSV,encoding='utf-8')}
o=open(TSV,'a',encoding='utf-8'); k=len(done); made=0
for rel,kind,src,entry in targets:
    if rel in done: continue
    if time.time()-t0>budget: break
    tb=open(G+rel,'rb').read(); tm=md5(tb); sm=''; ps=0
    if kind!='new':
        try: sb,ename=(disc_bytes(src),'') if kind=='disc' else big_bytes(src,entry)
        except KeyError: kind,src,entry='new','',''
    if kind!='new':
        sm=md5(sb); sp,tp=OUT+'/tmp/s.bin',OUT+'/tmp/t.bin'; open(sp,'wb').write(sb); open(tp,'wb').write(tb)
        pp=OUT+'/patches/%04d.vcdiff'%k
        r=subprocess.run([XD,'-e','-9','-f','-S','djw','-s',sp,tp,pp],capture_output=True)
        if r.returncode: print('xdelta fail',rel,r.stderr[:200]); continue
        ps=os.path.getsize(pp); patch='%04d.vcdiff'%k; k+=1
    else: patch=''
    o.write('\t'.join([rel,kind,src,entry,sm,tm,str(len(tb)),str(ps),patch])+'\n'); o.flush(); made+=1
print('targets %d, done now %d, total %d'%(len(targets),made,len(done)+made))
