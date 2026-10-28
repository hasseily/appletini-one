"""Keep selected integrity evidence separate from the UART harness journal."""
from pathlib import Path
import hashlib,json,shutil
ROOT=Path(__file__).resolve().parents[1]
def main():
    source=ROOT/'build/rsa-stress';j=json.loads((source/'journal.json').read_text())
    assert j['pass'] and j['restored'] and len(j['cases'])==2
    out=ROOT/'validation/rsa-stress-20261028';out.mkdir(parents=True,exist_ok=True)
    report={'pass':True,'acceptance':'Stutter is allowed; no data, filesystem or capture corruption; no reported USB errors; recovery required',
        'normal_firmware_before':j['original_firmware_sha256'],'instrumented_firmware':j['diagnostic_sha256'],
        'fpga_rebuilt':False,'golden_written':False,'cases':j['cases'],'original_profile_restored':True,
        'limitations':['USB report counts and error counters do not provide a known input-sequence oracle.',
          'No physical HDMI frame-loss measurement; compositor stutter is accepted.',
          'Writes target only the two owned scratch volumes; this is a bounded two-speed stress run.'],
        'harness_history':'The first attempt stopped on a UART echo parser error; the parser regression passes and both complete runs were repeated with fresh images.'}
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    for name in ('client-build.json','expected.json','full-uart.txt','turbo-uart.txt'):shutil.copy2(source/name,out/name)
    shutil.copy2(source/'inputs/diagnostic-build.json',out/'diagnostic-build.json')
    shutil.copy2(source/'source-checks/results.json',out/'source-checks.json')
    hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir() if p.is_file() and p.name!='SHA256SUMS.json'}
    (out/'SHA256SUMS.json').write_text(json.dumps(hashes,indent=2)+'\n')
    print('Recorded verified RSA stress evidence:',out)
if __name__=='__main__':main()
