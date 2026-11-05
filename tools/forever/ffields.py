import fupdate as upd,struct,collections,sys
from fwl import load
R=upd.R
def unit(d,blk):
    r=R(d,blk['fieldpos']); r.u8()                    # indirect fragment active
    o={'entry':r.i32(),'dyn':r.u32(),'scale':r.f()}
    o['display']=r.i32(); o['npcflags']=r.u32(); r.u32(); r.u32(); r.u32(); r.u32()
    n=r.u32(); r.u32(); r.i32()
    for _ in range(n): r.u32()
    for _ in range(2): r.guid()                       # charm, summon
    for _ in range(7): r.guid()                       # charmedby..BattlePetCompanion
    r.u64(); r.guid(); r.guid()
    r.i32(); r.i32(); r.i32(); r.u32(); r.u32()       # channel
    r.u8()
    r.u32(); o['race']=r.u8(); o['class']=r.u8(); r.u8(); o['sex']=r.u8(); o['ctype']=r.u8(); r.u8(); r.u32()
    o['hp']=struct.unpack('<q',r.raw(8))[0]
    for _ in range(20): r.i32()
    o['maxhp']=struct.unpack('<q',r.raw(8))[0]
    o['level']=r.i32(); r.i32(); r.i32(); r.i32(); r.i32(); r.i32(); r.u8()
    o['faction']=r.i32()
    return o
def go(d,blk):
    r=R(d,blk['fieldpos']); r.u8()
    o={'entry':r.i32(),'dyn':r.u32(),'scale':r.f()}
    o['display']=r.i32(); r.u32(); r.u32(); r.u32(); r.u32()
    n=r.u32(); r.u32()
    for _ in range(n): r.u32()
    r.guid(); r.guid(); o['flags']=r.u32(); r.u32()
    for _ in range(4): r.f()
    o['faction']=r.i32(); r.i32(); o['state']=struct.unpack('b',bytes([r.u8()]))[0]; o['gotype']=struct.unpack('b',bytes([r.u8()]))[0]
    return o
if __name__=='__main__':
    pk=load(sys.argv[1]); seen={}
    for dr,op,ms,b in pk:
        if op!=0x5E0000: continue
        m,bl=upd.parse_update(b)
        for x in bl:
            if x['type']!=1: continue
            try:
                if x['objtype']==5: o=unit(b,x)
                elif x['objtype']==8: o=go(b,x)
                else: continue
            except Exception as e: print('ERR',x['objtype'],repr(e)); continue
            pos=x['mv'].get('pos') or x['mv'].get('stat')
            print(x['objtype'],'entry',o['entry'],'disp',o['display'],'lvl',o.get('level'),'hp',o.get('hp'),o.get('maxhp'),'fac',o['faction'],'scale',round(o['scale'],2),'pos',tuple(round(v,1) for v in pos) if pos else None, 'npc',o.get('npcflags'), 'type',o.get('gotype'))
