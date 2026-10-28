"""Small UART helpers for the HTTP application acceptance checks."""
from pathlib import Path
import ftplib,re,serial,time
from smartport_timing_board import serial_command,get,put

class Board:
    def __init__(self,out):self.out=Path(out);self.out.mkdir(parents=True,exist_ok=True);self.logs=[]
    def __enter__(self):self.uart=serial.Serial('COM13',921600,timeout=.01);return self
    def __exit__(self,*args):self.uart.close()
    def command(self,text,timeout=.35):
        value=serial_command(self.uart,text,timeout=timeout);self.logs.append(value)
        (self.out/'uart.txt').write_text(''.join(self.logs),encoding='utf-8');return value
    def until(self,command,needle,seconds=40):
        value=self.command(command,.3);deadline=time.monotonic()+seconds
        while needle not in value and time.monotonic()<deadline:
            part=self.uart.read(max(1,self.uart.in_waiting)).decode('utf-8',errors='replace')
            if part:value+=part;self.logs.append(part)
        (self.out/'uart.txt').write_text(''.join(self.logs),encoding='utf-8')
        assert needle in value,'UART response deadline: '+command
        return value
    def dump(self,address,n):
        # Startup HID messages can delay a read-only UART command. Retry only
        # reads; never replay an unacknowledged byte write or key press.
        for attempt in range(8):
            value=self.command(f'vtw dump {address:X} {n:X}',1);data={}
            for m in re.finditer(r'(?m)^([0-9A-F]{5}):((?: [0-9A-F]{2})+)',value.replace('\r','')):
                for i,b in enumerate(m[2].split()):data[int(m[1],16)+i]=int(b,16)
            if all(address+i in data for i in range(n)):return bytes(data[address+i] for i in range(n))
        raise RuntimeError('Incomplete UART memory read: '+value)
    def mwr(self,address,value):
        result=self.command(f'mwr 0x{address:X} 0x{value:X}',.1);assert 'ok' in result,result
    def poke(self,address,data):
        self.mwr(0x400001C4,address)
        for b in data:self.mwr(0x400001C8,b)
    def key(self,key):self.mwr(0x40000170,ord(key) if isinstance(key,str) else key)
    def wait(self,address,values,seconds=30):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            value=self.dump(address,1)[0]
            if value in values:return value
        raise RuntimeError(f'RAM {address:04X} did not reach {values}; last={value}')
    def reboot(self,require_http=True):
        self.command('reboot',.2);deadline=time.monotonic()+100
        while time.monotonic()<deadline:
            text=self.command('status',.5)
            if '---- card status ----' in text:
                if not require_http:return text
                for _ in range(20):
                    result=self.command('http status',1)
                    if '[http-status]' in result:return result
        raise RuntimeError('Frontend boot deadline')
    def ftp(self,fn):
        assert 'ftp: on' in self.command('ftp on',6)
        try:
            with ftplib.FTP() as ftp:
                ftp.connect('10.17.1.18',timeout=15);ftp.login('anonymous','http-acceptance');return fn(ftp)
        finally:self.command('ftp off',1)
    def select(self,path):
        def update(ftp):
            profile=get(ftp,'/appletini_cfg.txt').decode()
            profile=re.sub(r'(?m)^smartport.disk.1.path=[^\r\n]*','smartport.disk.1.path="'+path+'"',profile)
            put(ftp,'/appletini_cfg.txt',profile.encode())
        self.ftp(update);return self.reboot()
