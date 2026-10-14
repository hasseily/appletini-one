import profile_harness as h

c = h.connect()
try:
    entry = h.prepare(c,'TARGET PRACTICE')
    c.bp_clear_all()
    h.breakpoint(c,0xfd0c)
    c.continue_()
    h.wait_pause(c,30,'TARGET terrain, unmeasured repeat')
    assert h.registers(c)['pc']==0xfd0c
    c.bp_clear_all()
    h.breakpoint(c,0xd7d2)
    c.paste_text('45,20\r')
    c.continue_()
    h.wait_pause(c,30,'TARGET shot input')
    assert h.registers(c)['pc']==0xd7d2
    h.capture_current(c,entry,'TARGET_PRACTICE_SHOT',limit=100_000_000,
        keyboard_stop=True,boundary_note=
        'NEWSTT after angle=45,power=20; fixed-seed terrain; through next input request')
finally:
    c.close()
