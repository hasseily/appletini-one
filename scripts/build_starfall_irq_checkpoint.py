"""Build a runtime probe for the exact copied STARFALL program."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'software/starfall_irq_checkpoint'
OUT=ROOT/'build/starfall-irq-checkpoint'
sys.path.insert(0,str(ROOT/'software/net_transport_checkpoint'))
from prodos_disk import read_root_files,validate_image


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    inputs=SOURCE/'inputs'
    labels={line.split()[2].lstrip('.'):int(line.split()[1],16)
            for line in (inputs/'starfall.lbl').read_text().splitlines()}
    program=(inputs/'STARFALL.SYSTEM').read_bytes()
    irq=labels['_ti_audio_irq'];update=labels['_ti_audio_update']
    irq_prefix=program[irq-0x2000:irq-0x2000+4]
    prologue=program[update-0x2000:update-0x2000+3]
    assert irq_prefix[:2]==b'\xD8\xAD' and prologue[0]==0x20
    game={'GAME_IRQ':irq,'GAME_CLOCK_RUNNING':int.from_bytes(irq_prefix[2:],'little'),
          'GAME_UPDATE':update,'GAME_AUDIO_PROLOGUE':int.from_bytes(prologue[1:],'little'),
          'GAME_AUDIO_COUNTER':labels['_ti_clock_counter'],'GAME_LATCH':labels['_ti_clock_latch'],
          'GAME_FRAMES':labels['_sf_frames']}
    (OUT/'game.inc').write_text(''.join(f'{key}=${value:04X}\n' for key,value in game.items()))
    subprocess.run(['ca65','-I',str(OUT),str(SOURCE/'probe.s'),'-o',str(OUT/'probe.o')],check=True)
    subprocess.run(['ld65','-C',str(SOURCE/'probe.cfg'),str(OUT/'probe.o'),
                    '-o',str(OUT/'probe.bin'),'-Ln',str(OUT/'probe.lbl')],check=True)
    probe_labels={line.split()[2].lstrip('.'):int(line.split()[1],16)
                  for line in (OUT/'probe.lbl').read_text().splitlines()}
    image=(inputs/'STARFALL.PO').read_bytes()
    validate_image(image)
    files=read_root_files(image)
    assert files['STARFALL.SYSTEM']==program
    assert files['STARFALL.PAK']==(inputs/'STARFALL.PAK').read_bytes()
    (OUT/'STARFALL.PO').write_bytes(image)
    sha=lambda data:hashlib.sha256(data).hexdigest()
    report={'game':game,'probe_labels':probe_labels,'irq_prefix':irq_prefix.hex(),
            'update_prefix':prologue.hex(),'probe_bytes':(OUT/'probe.bin').stat().st_size,
            'game_sha256':sha(program),'disk_sha256':sha(image),
            'probe_sha256':sha((OUT/'probe.bin').read_bytes()),
            'limits':{'code':[0xA280,0xA900],'data':[0xA900,0xAA00],
                      'buffer':[0xAA00,0xAC02],'scene_start':0xAC30},
            'restriction':'sector 1 only; restart before each short phase; no boss overlay'}
    (OUT/'client-build.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS exact STARFALL input, probe layout, owned disk; probe bytes:',report['probe_bytes'])


if __name__=='__main__':main()
