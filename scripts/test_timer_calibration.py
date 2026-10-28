"""Check the opt-in ARM timestamp packet without running board code."""
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
from test_onee_input_service import find_native_c_compiler

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/starfall-irq-checkpoint/host'


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/'xiltimer.h').write_text('#include <stdint.h>\ntypedef uint64_t XTime;\n'
        '#define COUNTS_PER_SECOND 333333343U\n'
        'static inline void XTime_GetTime(XTime *v) {*v=UINT64_C(0x0123456789ABCDEF);}\n')
    cc=find_native_c_compiler();assert cc
    if os.name=='nt':dll_path=os.add_dll_directory(str(cc.parent))
    dll=OUT/('stamp.dll' if os.name=='nt' else 'stamp.so')
    subprocess.run([str(cc),'-std=c11','-O2','-Wall','-Wextra','-Werror','-shared',
                    '-DAPPLETINI_NET_TRANSPORT_TEST=1','-DAPPLETINI_TIMER_CALIBRATION=1',
                    '-I'+str(OUT),str(ROOT/'ps_sources/frontend/net_transport_test.c'),'-o',str(dll)],check=True)
    module=ctypes.CDLL(str(dll))
    module.net_transport_test_status.argtypes=[ctypes.c_uint8,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t]
    module.net_transport_test_status.restype=ctypes.c_uint16
    params=bytes([3,0,0,0,0x82,0xA7,0xCA,32,0]);reply=(ctypes.c_ubyte*34)(*([0xCC]*34))
    assert module.net_transport_test_status(0x82,params,reply,31)==0
    assert bytes(reply)==bytes([0xCC]*34)
    assert module.net_transport_test_status(0x82,params,reply,32)==32
    assert bytes(reply[:4])==b'ATTC'
    assert struct.unpack('<QI',bytes(reply[4:16]))==(0x0123456789ABCDEF,333333343)
    assert bytes(reply[16:32])==bytes(16) and bytes(reply[32:])==b'\xCC\xCC'
    normal=bytes([3,0,0,0,0x82,0,0,32,0])
    assert module.net_transport_test_status(0x82,normal,reply,32)==32
    assert bytes(reply[:4])==b'ATNT'
    (OUT/'result.json').write_text(json.dumps({'pass':True,'timestamp_64bit':True,
        'frequency':333333343,'normal_descriptor_unchanged':True,'bounds_checked':True},indent=2)+'\n')
    print('PASS ARM timestamp width/frequency/bounds and unchanged normal descriptor')


if __name__=='__main__':main()
