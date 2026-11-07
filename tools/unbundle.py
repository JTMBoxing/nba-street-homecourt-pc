# Unpack capture/frameN/bundle.bin (written by gpu.cpp) into a folder: python3 unbundle.py bundle.bin outdir
import os, struct, sys
src, out = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
i = n = 0
while i < len(data):
    (ln,) = struct.unpack_from('<I', data, i); i += 4
    name = data[i:i + ln].decode(); i += ln
    (sz,) = struct.unpack_from('<I', data, i); i += 4
    path = os.path.join(out, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(data[i:i + sz]); i += sz; n += 1
print(n, 'files')
