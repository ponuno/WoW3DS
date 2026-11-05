import struct,sys
def load(path):
    d=open(path,'rb').read(); assert d[:4]==b'FWL1'; i=4; out=[]
    while i<len(d):
        dr,op,ms,sz=struct.unpack('<BIII',d[i:i+13]); i+=13
        out.append((dr,op,ms,d[i:i+sz])); i+=sz
    return out
if __name__=='__main__':
    pk=load(sys.argv[1]); 
    for n,(dr,op,ms,b) in enumerate(pk):
        if op==0x5E0000: print(n,ms,len(b),b[:48].hex())
