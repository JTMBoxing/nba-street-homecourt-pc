# isocmp.py <game.iso> <game folder> <out manifest.tsv> [seconds]
# Reads the Xbox 360 disc image's XDVDFS file table (no extraction), and for every file on the disc writes:
#   path  size  md5-on-disc  md5-in-game-folder (or MISSING)  SAME/CHANGED
# Resumable: files already in the manifest are skipped, stops after [seconds] so it fits one shell call.
import sys,os,struct,hashlib,time
iso,gamedir,out=sys.argv[1:4]; budget=float(sys.argv[4]) if len(sys.argv)>4 else 160; t0=time.time()
f=open(iso,'rb'); SEC=2048
base=None
for off in (0x0, 0xFD90000, 0x2080000, 0x18300000):      # plain XISO, XGD2, XGD3, XGD1 game partition
    f.seek(off+32*SEC); h=f.read(0x14)
    if h==b'MICROSOFT*XBOX*MEDIA': base=off; break
assert base is not None,'no XDVDFS header found'
f.seek(base+32*SEC+0x14); rootsec,rootsize=struct.unpack('<II',f.read(8))
files=[]
def walk(sec,size,prefix):
    f.seek(base+sec*SEC); data=f.read(size)
    def node(o):
        if o*4+14>len(data): return
        l,r,fs,fz,attr,nl=struct.unpack_from('<HHIIBB',data,o*4)
        if l==0xFFFF: return
        name=data[o*4+14:o*4+14+nl].decode('latin-1')
        if l: node(l)
        if attr&0x10:
            if fz: walk(fs,fz,prefix+name+'/')
        else: files.append((prefix+name,fs,fz))
        if r: node(r)
    node(0)
walk(rootsec,rootsize,'/')
done=set()
if os.path.exists(out): done={l.split('\t')[0] for l in open(out,encoding='utf-8') if l.strip()}
def md5f(fh,start,n):
    m=hashlib.md5(); fh.seek(start); left=n
    while left>0:
        b=fh.read(min(8<<20,left)); m.update(b); left-=len(b)
        if not b: break
    return m.hexdigest()
o=open(out,'a',encoding='utf-8'); k=0
for p,sec,sz in sorted(files):
    if p in done: continue
    if time.time()-t0>budget: break
    a=md5f(f,base+sec*SEC,sz); g=os.path.join(gamedir,p.lstrip('/'))
    if os.path.exists(g):
        with open(g,'rb') as gh: b=md5f(gh,0,os.path.getsize(g))
    else: b='MISSING'
    o.write('%s\t%d\t%s\t%s\t%s\n'%(p,sz,a,b,'SAME' if a==b else 'CHANGED')); o.flush(); k+=1
print('disc files %d, done now %d, total done %d'%(len(files),k,len(done)+k))
