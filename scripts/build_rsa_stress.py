"""Build the moving-video and ProDOS write/readback workload."""
from pathlib import Path
import hashlib,json,struct,subprocess,sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/rsa-stress'
sys.path.insert(0,str(ROOT/'software/net_transport_checkpoint'))
from prodos_disk import build_image,validate_image
def main():
    OUT.mkdir(parents=True,exist_ok=True);source=ROOT/'software/rsa_stress/main.c'
    subprocess.run(['D:/Repos/cc65/bin/cl65.exe','-t','apple2enh','-O','--start-addr','0x2000',
        '-Wl','-D,__FILETYPE__=0xFF','-Ln',str(OUT/'client.lbl'),'-m',str(OUT/'client.map'),
        '-o',str(OUT/'client.as'),str(source)],cwd=OUT,check=True)
    raw=(OUT/'client.as').read_bytes();assert raw[:8]==bytes.fromhex('0005160000020000')
    entries={kind:raw[at:at+n] for kind,at,n in (struct.unpack_from('>III',raw,26+12*i) for i in range(int.from_bytes(raw[24:26],'big')))}
    assert entries[11]==bytes.fromhex('00c300ff00002000');program=entries[1];assert program[:3]==bytes.fromhex('a2ff9a')
    (OUT/'RSATEST.SYSTEM').write_bytes(program)
    template=(ROOT/'software/ProDOS_2_4_3.po').read_bytes()
    guard=bytes((i*37+91)&255 for i in range(8192))
    volumes={}
    for volume,blocks in [('RSAHOST',1600),('RSADISK',280)]:
        image=build_image(template,program,'RSATEST.SYSTEM',total=blocks,volume=volume,
            files=[('SCRATCH',bytes(32768),0x06,0),('GUARD',guard,0x06,0)])
        validate_image(image);(OUT/(volume+'.PO')).write_bytes(image);volumes[volume]=hashlib.sha256(image).hexdigest()
    labels={line.split()[2].lstrip('.'):int(line.split()[1],16) for line in (OUT/'client.lbl').read_text().splitlines()}
    (OUT/'client-build.json').write_text(json.dumps({'program_sha256':hashlib.sha256(program).hexdigest(),
        'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'volumes':volumes,'labels':labels},indent=2)+'\n')
    print('PASS stress client build, isolated volumes and ProDOS structure')
if __name__=='__main__':main()
