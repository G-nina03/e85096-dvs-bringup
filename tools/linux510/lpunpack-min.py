#!/usr/bin/env python3
"""lpunpack-min.py super.img [NAME...] - list the logical partitions of an Android super image
(LP metadata, slot 0) and write NAME.img for each NAME given. Linear extents only, no deps."""
import struct,sys
f=open(sys.argv[1],'rb')
f.seek(4096); g=f.read(52)
magic,sz,chk,max_md,slots,blk=struct.unpack('<II32sIII',g)
print('geom %x max_md %d slots %d'%(magic,max_md,slots))
f.seek(4096*3)  # primary metadata slot0 after 2 geometry copies
h=f.read(256)
m,maj,mn,hsz=struct.unpack_from('<IHHI',h); print('hdr %x v%d.%d'%(m,maj,mn))
# header: magic,major,minor,header_size,header_checksum[32],tables_size,tables_checksum[32], partitions desc(off,num,size), extents, groups, block_devices
off=4+2+2+4+32+4+32
tabs=[struct.unpack_from('<III',h,off+12*i) for i in range(4)]
base=4096*3+hsz
f.seek(base); T=f.read(struct.unpack_from('<I',h,44)[0])
parts=[];ext=[]
for i in range(tabs[0][1]):
    p=T[tabs[0][0]+i*tabs[0][2]:][:tabs[0][2]]
    name=p[:36].split(b'\0')[0].decode(); attr,fi,ne,grp=struct.unpack_from('<IIII',p,36); parts.append((name,fi,ne))
for i in range(tabs[1][1]):
    e=T[tabs[1][0]+i*tabs[1][2]:][:tabs[1][2]]
    ext.append(struct.unpack_from('<QIQI',e))
for name,fi,ne in parts:
    es=ext[fi:fi+ne]; print(name,es)
    if name in sys.argv[2:]:
        with open(name+'.img','wb') as o:
            for nsec,typ,tdata,dev in es:
                f.seek(tdata*512); left=nsec*512
                while left: b=f.read(min(left,1<<24)); o.write(b); left-=len(b)
