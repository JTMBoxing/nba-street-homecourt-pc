# modclass.py <port folder> <out.tsv> : every file under game/mods: is a file of the same name inside the disc's .big archives?
# (same name = an edited copy of one of the game's own files -> must ship as a patch, not as a file)
import sys,os,hashlib
P=sys.argv[1]; sys.path.insert(0,os.path.join(P,'dev','tools'))
from meshlib import ents
names={}
for root,_,fs in os.walk(os.path.join(P,'game','data')):
    for fn in fs:
        if fn.lower().endswith('.big') and '.before' not in fn:
            try:
                f,es=ents(os.path.join(root,fn))
                for name,off,size in es: names.setdefault(name.rsplit('/',1)[-1].lower(),(os.path.relpath(os.path.join(root,fn),P),off,size))
            except Exception as e: print('skip',fn,e)
print('names in archives',len(names))
o=open(sys.argv[2],'w')
for root,_,fs in os.walk(os.path.join(P,'game','mods')):
    for fn in fs:
        p=os.path.relpath(os.path.join(root,fn),P); hit=names.get(fn.lower())
        o.write('%s\t%d\t%s\n'%(p,os.path.getsize(os.path.join(root,fn)),'EDIT-OF '+hit[0] if hit else 'NEW'))
