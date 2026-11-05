import struct,sys,zlib
from fwl import load
class R:
    def __init__(s,b,p=0): s.b=b;s.p=p;s.bit=0;s.cur=0
    def flush(s): s.bit=0
    def bits(s,n):
        v=0
        for _ in range(n):
            if s.bit==0: s.cur=s.b[s.p]; s.p+=1; s.bit=8
            s.bit-=1; v=(v<<1)|((s.cur>>s.bit)&1)
        return v
    def u8(s): s.flush(); v=s.b[s.p]; s.p+=1; return v
    def u16(s): s.flush(); v=struct.unpack_from('<H',s.b,s.p)[0]; s.p+=2; return v
    def u32(s): s.flush(); v=struct.unpack_from('<I',s.b,s.p)[0]; s.p+=4; return v
    def i32(s): s.flush(); v=struct.unpack_from('<i',s.b,s.p)[0]; s.p+=4; return v
    def u64(s): s.flush(); v=struct.unpack_from('<Q',s.b,s.p)[0]; s.p+=8; return v
    def f(s): s.flush(); v=struct.unpack_from('<f',s.b,s.p)[0]; s.p+=4; return v
    def raw(s,n): s.flush(); v=s.b[s.p:s.p+n]; s.p+=n; return v
    def guid(s):
        s.flush(); m=s.u8()|(s.u8()<<8); g=bytearray(16)
        for i in range(16):
            if m>>i&1: g[i]=s.u8()
        return bytes(g)
FL=['EntityPos','NoBirthAnim','EnablePortals','PlayHoverAnim','ThisIsYou','MovementUpdate','MovementTransport','Stationary','CombatVictim','ServerTime','Vehicle','AnimKit','Rotation','GameObject','SmoothPhasing','SceneObject','ActivePlayer','Conversation','Room','Decor','MeshObject']
def gname(g): return g.hex()
def movement(r):
    fl={n for n in FL if r.bits(1)}; r.flush()
    pause=r.u32(); out={'flags':fl}
    for _ in range(pause): r.u32()
    if 'MovementUpdate' in fl:
        out['mover']=r.guid(); out['mflags']=r.u64(); r.u32()
        out['pos']=(r.f(),r.f(),r.f(),r.f()); r.f(); r.f(); nrf=r.u32(); r.u32(); r.f()
        for _ in range(nrf): r.guid()
        bsog,btr,bfall,bspl,_h,_rt,binert,badv,bdrv=[r.bits(1) for _ in range(9)]; r.flush()
        if bsog: r.guid()
        if btr: out['transport']=(r.guid(),r.f(),r.f(),r.f(),r.f(),r.i32(),r.u32()); r.bits(1)  # (approximate)
        if bfall:
            r.u32(); r.f(); 
            if r.bits(1): r.f();r.f();r.f()
            r.flush()
        assert not binert and not badv and not bdrv,'inertia/advfly/drive'
        out['speeds']=[r.f() for _ in range(9)]
        nf=r.u32(); r.f()
        for _ in range(17): r.f()
        assert nf==0
        hs=r.bits(1); r.flush()
        if hs:
            out['splineId']=r.u32(); out['dest']=(r.f(),r.f(),r.f()); mv=r.bits(1); r.flush()
            if mv:
                out['sflags']=r.u32(); face=r.u8(); r.i32(); out['dur']=r.u32(); r.f(); r.f()
                if face==1: r.f();r.f();r.f()
                elif face==2: r.guid()
                elif face==3: r.f()
                fade=r.bits(1); n=r.bits(16); filt=r.bits(1); sx=r.bits(1); jmp=r.bits(1); turn=r.bits(1); tier=r.bits(1); vis=r.bits(1); r.flush()
                assert not filt and not vis
                if fade: r.u32()
                out['path']=[(r.f(),r.f(),r.f()) for _ in range(n)]
                if sx: r.guid(); r.u32();r.u32();r.u32();r.f()
                if jmp: r.f();r.u32();r.u32()
                if turn: r.f();r.f();r.f()
                if tier: r.i32();r.u8();r.u32();r.u32()
    if 'MovementTransport' in fl and 'transport' not in out: raise Exception('transport block')
    if 'Stationary' in fl: out['stat']=(r.f(),r.f(),r.f(),r.f())
    if 'CombatVictim' in fl: r.guid()
    if 'ServerTime' in fl: r.u32()
    if 'Vehicle' in fl: r.u32(); r.f()
    if 'AnimKit' in fl: r.u16();r.u16();r.u16()
    if 'Rotation' in fl: out['rot']=r.u64()
    if 'GameObject' in fl:
        r.u32(); b8=r.bits(1); tr=r.bits(1); pp=r.bits(1); r.flush()
        if tr: r.u32(); r.u32(); r.bits(3); r.flush()
        if b8: r.u32()
        if pp: r.f()
    if 'ActivePlayer' in fl:
        hsc=r.bits(1); hrn=r.bits(1); r.flush()
        if hsc:
            for _ in range(r.u32()): r.u32()
        if hrn:
            r.u8(); r.u8(); mr=r.u32()
            for _ in range(mr): r.u8()
    assert not (fl & {'SmoothPhasing','SceneObject','Conversation','Room','Decor','MeshObject'}),fl
    return out
def parse_update(d):
    r=R(d); m=r.u16(); n=r.u32(); r.bits(1); hd=r.bits(1); r.flush()
    if hd:
        nd=r.u16(); tot=r.u32(); 
        for _ in range(tot): r.guid()
    size=r.u32(); end=r.p+size; blocks=[]
    for i in range(n):
        t=r.u8(); 
        if t==0:   # values
            g=r.guid(); sz=r.u32(); r.p+=sz; blocks.append(dict(type=t,guid=g,size=sz)); continue
        g=r.guid(); ot=r.u8(); mv=movement(r); sz=r.u32(); ff=r.u8(); fr=[]
        start=r.p-1
        while True:
            x=r.u8(); 
            if x==0xFF: break
            fr.append(x)
        blocks.append(dict(type=t,guid=g,objtype=ot,mv=mv,fields=sz,fieldflags=ff,frags=fr,fieldpos=r.p,fend=start+sz+1+0))
        r.p=start+1+ (sz) if False else r.p  # field data parsed later
        # skip the rest of the block: size counts from after the u32 (flags byte included)
        r.p=start+sz
    assert r.p==end,(r.p,end,i)
    return m,blocks
if __name__=='__main__':
    pk=load(sys.argv[1]); 
    for n,(dr,op,ms,b) in enumerate(pk):
        if op!=0x5E0000: continue
        try: m,bl=parse_update(b)
        except Exception as e: print(n,'ERR',repr(e)); continue
        print(n,ms,'map',m,len(bl),'blocks')
        for x in bl[:6]:
            print('  ',x['type'],x['guid'].hex(),x.get('objtype'),sorted(x.get('mv',{}).get('flags',[])),x.get('mv',{}).get('stat') or x.get('mv',{}).get('pos'),x.get('frags'))
