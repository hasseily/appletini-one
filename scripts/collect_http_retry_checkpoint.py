"""Save a completed, restored HTTP retry run and its exact artifact hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'validation/http-network-retry-20261028')
    parser.add_argument('--expect-early-dns',action='store_true',
                        help='require a request within 2 seconds of frontend readiness and no ARP retry')
    args=parser.parse_args()
    source = ROOT / 'build/http-api-checkpoint'
    result = json.loads((source/'result.json').read_text())
    assert result.get('pass') and result.get('restored'), 'The board run must pass and restore first'
    build = json.loads((ROOT/'build/http-service-checkpoint/build-report.json').read_text())
    assert build['package']['sha256']==result['inputs']['FIRMWARE.BIN'], 'Build provenance differs from tested image'
    uart = (source/'uart.txt').read_text(encoding='utf-8')
    assert not re.search(r'http provision [0-9a-fA-F]{96}',uart), 'Do not archive a provisioning secret'
    transfers = []
    for line in uart.splitlines():
        if line.startswith('[http-result]'):
            transfers.append({k:int(v,16 if v.startswith('0x') else 10)
                for k,v in re.findall(r'(\w+)=(-?0x[0-9A-Fa-f]+|-?\d+)',line)})
    assert len(transfers)==4, transfers  # cold, warm, public and no DNS replies
    assert all(r['rc']==0 and r['verify']==0 and r['status']==200 for r in transfers[:3])
    assert transfers[3]['rc']==-111 and 30000000<=transfers[3]['elapsed_us']<31000000
    if args.expect_early_dns:
        assert result['settle_seconds']==0
        assert result['provisioned_reboot']
        assert result['dns_start_seconds_after_frontend_ready']<2
        assert '[http-stage] DNS ARP retry' not in uart
    out = args.output
    out.mkdir(parents=True,exist_ok=True)
    for name in ('result.json','uart.txt','retry-peer-current.log'):
        shutil.copyfile(source/name,out/name)
    shutil.copyfile(source/'inputs/manifest.json',out/'input-manifest.json')
    shutil.copyfile(ROOT/'build/http-service-checkpoint/build-report.json',out/'build-report.json')
    report = {'transfers':transfers,'dns_arp_retries':uart.count('[http-stage] DNS ARP retry'),
              'dns_reply_retries':uart.count('[http-stage] DNS reply retry'),
              'client':result['client'],'stock_restored':True,'fpga_rebuilt':False,
              'golden_written':False,'tcp_retry_evidence':'production engine with register model',
              'switch_configuration_read_by_test':False}
    if 'dns_start_seconds_after_frontend_ready' in result:
        report['dns_start_seconds_after_frontend_ready']=result['dns_start_seconds_after_frontend_ready']
        report['settle_seconds']=result['settle_seconds']
        report['provisioned_reboot']=result['provisioned_reboot']
        report['boots']=result['boots']
    if args.expect_early_dns:
        report['switch_edge_port']='GigabitEthernet1/0/3'
        report['switch_edge_configuration_source']='operator report'
    report['sources']={str(p.relative_to(ROOT)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest()
        for p in (ROOT/'ps_sources/frontend/http_service.c',ROOT/'ps_sources/frontend/http_service.h',
                  ROOT/'scripts/fixtures/http_network_host.c',ROOT/'scripts/test_http_network.py',
                  ROOT/'scripts/run_http_api_checkpoint.py')}
    (out/'summary.json').write_text(json.dumps(report,indent=2)+'\n',newline='\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':
    main()
