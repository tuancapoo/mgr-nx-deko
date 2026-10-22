import struct,sys
def decrypt(b):
    m=0x655f; out=bytearray(b)
    for i in range(len(out)):
        out[i]^=m&0xff; m=(m*0x4115)&0xffff
    return bytes(out)
def utf(b):
    if b[:4]!=b'@UTF': b=decrypt(b)
    assert b[:4]==b'@UTF', b[:4]
    size,rows_off,str_off,data_off,name_off=struct.unpack('>IIIII',b[4:24])
    ncol,rowlen,nrows=struct.unpack('>HHI',b[24:32])
    base=8
    def s(o):
        e=b.index(b'\0',base+str_off+o); return b[base+str_off+o:e].decode('utf8','replace')
    cols=[];p=32
    fmt={0:('B',1),1:('b',1),2:('H',2),3:('h',2),4:('I',4),5:('i',4),6:('Q',8),7:('q',8),8:('f',4),0xA:('I',4),0xB:('II',8)}
    def rd(t,p):
        f,n=fmt[t]; v=struct.unpack('>'+f,b[p:p+n]); 
        if t==0xA: return s(v[0]),n
        if t==0xB: return (v[0],v[1]),n
        return v[0],n
    for _ in range(ncol):
        fl=b[p]; nm=s(struct.unpack('>I',b[p+1:p+5])[0]); p+=5
        st=fl&0xF0; t=fl&0x0F; const=None
        if st==0x30: const,n=rd(t,p); p+=n
        cols.append((nm,st,t,const))
    rows=[]
    for r in range(nrows):
        p=base+rows_off+r*rowlen; row={}
        for nm,st,t,const in cols:
            if st==0x50: v,n=rd(t,p); p+=n; row[nm]=v
            elif st==0x30: row[nm]=const
            else: row[nm]=None
        rows.append(row)
    return rows,b,base+data_off
def toc(path):
    f=open(path,'rb'); h=f.read(0x800)
    assert h[:4]==b'CPK '
    ln=struct.unpack('<Q',h[8:16])[0]
    rows,_,_=utf(h[16:16+ln])
    hdr=rows[0]
    out=[]
    if hdr.get('TocOffset'):
        f.seek(hdr['TocOffset']); t=f.read(16); ln=struct.unpack('<Q',t[8:16])[0]
        trows,_,_=utf(f.read(ln))
        for r in trows:
            out.append((f"{r.get('DirName') or ''}/{r['FileName']}", r['FileOffset']+hdr['TocOffset'] if r['FileOffset']<hdr['TocOffset'] or True else 0, r['FileSize'], r['ExtractSize']))
    return hdr,out
if __name__=='__main__':
    hdr,files=toc(sys.argv[1])
    print({k:hdr[k] for k in ('TocOffset','ContentOffset','Files','Align') if k in hdr})
    import collections
    ext=collections.Counter(n.rsplit('.',1)[-1].lower() if '.' in n.rsplit('/',1)[-1] else '' for n,_,_,_ in files)
    print(len(files),'files', ext.most_common(25))
    comp=sum(1 for n,o,s,e in files if s!=e); print('compressed',comp)
    for n,o,s,e in files[:15]: print(n,o,s,e)
