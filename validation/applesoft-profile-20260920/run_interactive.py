"""Bounded game phases and a complete TESSELLATOR draw, excluding GET waits."""
import json
import profile_harness as h

inventory = json.loads((h.ROOT/'inventory/inventory.json').read_text())
def entry(name):
    return next(x for x in inventory['files'] if x['name']==name)

def accept_key(c, key):
    """Run unmeasured through current RDKEY and stop at its caller's return."""
    regs = h.registers(c)
    assert regs['pc'] == 0xfd0c, regs
    sp = regs['sp'] & 255
    low = c.read_mem(0, 0x100 | ((sp+1)&255), 1)[0]
    high = c.read_mem(0, 0x100 | ((sp+2)&255), 1)[0]
    destination = ((low | high<<8)+1)&65535
    c.bp_clear_all()
    h.breakpoint(c, destination)
    c.paste_text(key)
    c.continue_()
    h.wait_pause(c,30,'accept '+repr(key))
    assert h.registers(c)['pc'] == destination

c = h.connect()
try:
    for name in ['SNAKE GAME', 'STACKER', 'CONCENTRATION', 'DESERT ISLAND',
                 'DRIVING', 'FIFTEEN PUZZLE', 'FIREWORKS', 'GR-KANOID',
                 'JUNK DRAWER', 'MIND THE GAPS', 'TARGET PRACTICE',
                 'TESSELLATOR', 'TOWER OF HANOI', 'TYPING GAME']:
        runid = entry(name)['listing'].replace('.bas.txt','').replace('.','_')
        h.capture(c,name,100_000_000,True,runid+'_STARTUP')
        if name == 'TESSELLATOR':
            assert h.registers(c)['pc']==0xfd0c
            c.bp_clear_all()
            h.breakpoint(c,0xd7d2)  # Next statement, after INPUT accepts all fields.
            c.paste_text('64,2,0\r')
            c.continue_()
            h.wait_pause(c,30,'TESSELLATOR input')
            assert h.registers(c)['pc']==0xd7d2
            h.capture_current(c,entry(name),runid+'_RENDER',boundary_note=
                'NEWSTT after INPUT accepts P=64,N=2,T=0; through BASIC prompt')
        elif name in ['DESERT ISLAND','JUNK DRAWER','CONCENTRATION',
                      'FIFTEEN PUZZLE','TOWER OF HANOI']:
            keys = {'DESERT ISLAND':'TEEEEETWD',
                    'JUNK DRAWER':'1LLLLKKKK2JJJJIIII',
                    'CONCENTRATION':'AB','FIFTEEN PUZZLE':'LKJI',
                    'TOWER OF HANOI':'12'}[name]
            for index,key in enumerate(keys):
                if h.registers(c)['pc']!=0xfd0c:
                    break
                accept_key(c,key)
                h.capture_current(c,entry(name),f'{runid}_KEY_{index+1:02d}',
                    limit=100_000_000,keyboard_stop=True,boundary_note=
                    f'After RDKEY accepted key {key!r}; through next RDKEY or BASIC prompt')
finally:
    c.close()
