"""Package copied A2LYNX programs into a standalone acceptance disk."""
from pathlib import Path
import hashlib,json,struct,sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'software/net_transport_checkpoint'))
from prodos_disk import build_image
def main():
    inputs=ROOT/'build/a2lynx-acceptance/inputs';out=inputs.parent
    template=(inputs/'ProDOS_2_4_3.po').read_bytes()
    assert hashlib.sha256(template).hexdigest()=='398d333cb2ab92df9f8bb2cf64b946f2567116910eb8359cf4bdee5d4194f0fa'
    programs={p.name:p.read_bytes() for p in inputs.glob('*.SYSTEM')};assert {'A2BROWSE.SYSTEM','A2LEGACY.SYSTEM'}<=programs.keys()
    def data_fork(data):
        if data[:4]!=bytes.fromhex('00051600'):return data
        assert data[4:8]==bytes.fromhex('00020000')
        entries=[struct.unpack_from('>III',data,26+12*i) for i in range(struct.unpack_from('>H',data,24)[0])]
        forks=[(at,n) for kind,at,n in entries if kind==1];assert len(forks)==1
        at,n=forks[0];assert at>=26+12*len(entries) and at+n==len(data)
        return data[at:at+n]
    forks={name:data_fork(data) for name,data in programs.items()}
    disk=build_image(template,forks['A2BROWSE.SYSTEM'],'A2BROWSE.SYSTEM',
        files=[('A2LEGACY.SYSTEM',forks['A2LEGACY.SYSTEM'],0xFF,0x2000)],volume='A2LYNX',max_program_bytes=0x9F00)
    (out/'A2LYNX.PO').write_bytes(disk)
    (out/'inputs.json').write_text(json.dumps({'inputs':{n:hashlib.sha256(b).hexdigest() for n,b in programs.items()},'disk_sha256':hashlib.sha256(disk).hexdigest()},indent=2)+'\n')
    print('Built standalone A2LYNX.PO',len(disk),'bytes')
if __name__=='__main__':main()
