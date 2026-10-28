"""UART command echoes may precede complete foreground result records."""
from run_rsa_stress import result_rows,phase_rows
def main():
    text=('cmd> vtw dum[http-result] rc=0 verify=0x00000000 status=200 body=603 handshake_us=687627\r\n'
          'p 3470 1\r\ncmd> [http-result] rc=-9984 verify=0x00000004 status=0 body=0\r\n'
          'cmd> [http-phase] name=https health=1 gaps=0 hid=42 xbox=73\r\n')
    rows=result_rows(text);assert len(rows)==2 and rows[0]['handshake_us']==687627
    assert rows[0]['verify']==0 and rows[1]['rc']==-9984 and rows[1]['verify']==4
    assert phase_rows(text)==[{'name':'https','health':1,'gaps':0,'hid':42,'xbox':73}]
    print('PASS stress UART records with command echoes, signed failures and verification flags')
if __name__=='__main__':main()
