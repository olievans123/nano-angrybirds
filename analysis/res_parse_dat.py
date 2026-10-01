#!/usr/bin/env python3
"""(resources-spec) Parse and validate Angry Birds 1.0 sprite-sheet / font / text .dat files.
usage: res_parse_dat.py [-v]"""
import struct, sys, os, glob
D='v1.0/Payload/AngryBirds.app/data/'
V='-v' in sys.argv
class R:
    def __init__(s,b): s.b=b; s.p=0
    def u8(s): v=s.b[s.p]; s.p+=1; return v
    def s16(s): v=struct.unpack_from('>h',s.b,s.p)[0]; s.p+=2; return v
    def u16(s): v=struct.unpack_from('>H',s.b,s.p)[0]; s.p+=2; return v
    def s32(s): v=struct.unpack_from('>i',s.b,s.p)[0]; s.p+=4; return v
    def utf(s):
        n=s.u16(); v=s.b[s.p:s.p+n]; s.p+=n; return v
    def left(s): return len(s.b)-s.p
def png_size(path):
    b=open(path,'rb').read(64)
    assert b[:8]==b'\x89PNG\r\n\x1a\n'
    first=b[12:16]
    if first==b'CgBI':
        # CgBI chunk (len 4) precedes IHDR
        w,h=struct.unpack('>II',b[32:40]); bd,ct=b[40],b[41]
    else:
        w,h=struct.unpack('>II',b[16:24]); bd,ct=b[24],b[25]
    return w,h,first.decode(),bd,ct
def pvr_info(path):
    b=open(path,'rb').read(52)
    f=struct.unpack('<13I',b)
    return dict(hdr=f[0],h=f[1],w=f[2],mips=f[3],flags=f[4],datasize=f[5],bpp=f[6],rmask=f[7],gmask=f[8],bmask=f[9],amask=f[10],magic=struct.pack('<I',f[11]),nsurf=f[12],filesize=os.path.getsize(path))
def sheet(path):
    r=R(open(path,'rb').read())
    img=r.utf().decode()
    n=r.s16()
    sprites=[]
    for i in range(n):
        name=r.utf().decode()
        x,y,w,h,px,py=[r.s16() for _ in range(6)]
        sprites.append((name,x,y,w,h,px,py))
    return img,sprites,r.left()
def font(path):
    r=R(open(path,'rb').read())
    img=r.utf().decode()
    leading=r.s16(); tracking=r.s16(); n=r.s16()
    g=[]
    for i in range(n):
        code=r.u16(); x,y,w,h,base=[r.s16() for _ in range(5)]
        g.append((code,x,y,w,h,base))
    return img,leading,tracking,g,r.left()
def texts(path):
    b=open(path,'rb').read(); r=R(b)
    ver=r.u8(); locsize=r.s32(); p0=r.p
    nloc=r.u8(); locs=[r.utf().decode() for _ in range(nloc)]
    assert r.p==p0+locsize,(r.p,p0,locsize)
    nid=r.s16(); ids=[r.utf().decode() for _ in range(nid)]
    offs_pos=r.p
    offs=[r.s32() for _ in range(nloc)]
    out={}
    for i,l in enumerate(locs):
        # engine: skip(i*4); off=readInt(); skip(off)
        p=offs_pos+i*4+4+offs[i]
        rr=R(b); rr.p=p
        strs=[rr.utf() for _ in range(nid)]
        out[l]=(strs,rr.p)
    return ver,locsize,locs,ids,offs,out,len(b)
if __name__=='__main__':
    for f in sorted(glob.glob(D+'*.dat')):
        base=os.path.basename(f)
        if base.startswith('FONT_'):
            img,lead,trk,g,left=font(f)
            w,h,first,bd,ct=png_size(D+img)
            asc=max(x[5] for x in g); desc=max(x[4]-x[5] for x in g)
            bad=[x for x in g if x[1]<0 or x[2]<0 or x[1]+x[3]>w or x[2]+x[4]>h]
            print('%-28s FONT image=%s (%dx%d %s bd%d ct%d) leading=%d tracking=%d glyphs=%d trailing=%d maxAsc=%d maxDesc=%d height=%d oob=%d'%(base,img,w,h,first,bd,ct,lead,trk,len(g),left,asc,desc,asc+desc,len(bad)))
            print('    chars: '+''.join(chr(x[0]) if x[0]>32 else '<%d>'%x[0] for x in g))
            print('    codes: '+' '.join('%d'%x[0] for x in g))
            if V:
                for x in g: print('      U+%04X %r x=%d y=%d w=%d h=%d base=%d'%(x[0],chr(x[0]),*x[1:]))
        elif base.startswith('TEXTS_'):
            ver,locsize,locs,ids,offs,out,total=texts(f)
            print('%-28s TEXTS ver=%d localeBlockSize=%d locales=%s ids=%d offsets=%s filesize=%d'%(base,ver,locsize,locs,len(ids),offs,total))
            for l,(strs,end) in out.items():
                print('    locale %s: %d strings, block ends at %d'%(l,len(strs),end))
            for i,k in enumerate(ids):
                if V or i<6:
                    print('      %-32s '%k+' | '.join('%s=%r'%(l,out[l][0][i].decode('utf-8')) for l in locs))
            allbytes=b''.join(b''.join(out[l][0]) for l in locs)
            chars=sorted(set(allbytes.decode('utf-8')))
            print('    distinct chars used: '+repr(''.join(chars)))
            print('    non-ASCII: '+repr(''.join(c for c in chars if ord(c)>126)), ' has NUL/C080:', b'\xc0\x80' in allbytes, ' has newline:', '\n' in chars, ' has 4-byte/surrogate:', any(ord(c)>0xffff for c in chars))
        else:
            img,sp,left=sheet(f)
            p=D+img
            if img.lower().endswith('.png'):
                w,h,first,bd,ct=png_size(p); info='%dx%d PNG first=%s bitdepth=%d colortype=%d'%(w,h,first,bd,ct)
            else:
                i=pvr_info(p); w,h=i['w'],i['h']; info='%dx%d PVR hdr=%d mips=%d flags=0x%x datasize=%d bpp=%d masks=%x/%x/%x/%x magic=%r nsurf=%d filesize=%d'%(w,h,i['hdr'],i['mips'],i['flags'],i['datasize'],i['bpp'],i['rmask'],i['gmask'],i['bmask'],i['amask'],i['magic'],i['nsurf'],i['filesize'])
            bad=[s for s in sp if s[1]<0 or s[2]<0 or s[1]+s[3]>w or s[2]+s[4]>h]
            piv=[s for s in sp if not(0<=s[5]<=s[3] and 0<=s[6]<=s[4])]
            center=[s for s in sp if s[5]==s[3]//2 and s[6]==s[4]//2]
            print('%-28s SHEET image=%s (%s) sprites=%d trailing=%d out-of-bounds=%d pivot-outside-rect=%d pivot==(w//2,h//2):%d'%(base,img,info,len(sp),left,len(bad),len(piv),len(center)))
            if V:
                for s in sp: print('      %-40s x=%d y=%d w=%d h=%d pivot=(%d,%d)'%s)
