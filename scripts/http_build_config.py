"""Configure the normal CPU0 HTTP service from project-owned sources."""
from pathlib import Path
import re

def configure_http_service(root,diagnostics=False):
    root=Path(root);path=root/'vitis_workspace/frontend/src/UserConfig.cmake'
    text=path.read_text()
    def add(block,entries):
        nonlocal text
        pattern=r'set\(\s*'+block+r'\b(?P<body>[^)]*)\)'
        m=re.search(pattern,text,re.S)
        if not m:raise RuntimeError('Missing '+block)
        body=m['body'].rstrip()
        for entry in entries:
            if entry not in body:body+='\n'+entry
        text=text[:m.start()]+'set('+block+body+'\n)'+text[m.end():]
    definitions=['APPLETINI_HTTP_SERVICE=1','"MBEDTLS_CONFIG_FILE=<http_service_mbedtls_config.h>"']
    if diagnostics:definitions+=['APPLETINI_HTTP_DIAGNOSTICS=1']
    add('USER_COMPILE_DEFINITIONS',definitions)
    add('USER_INCLUDE_DIRECTORIES',['"../../../third_party/mbedtls/include"'])
    sources=['"../../../ps_sources/frontend/'+n+'"' for n in
             ('http_core.c','http_seed.c','http_service.c','http_bench.c','net_service.c','net_json.c')]
    sources+=['"../../../'+p.relative_to(root).as_posix()+'"' for p in sorted((root/'third_party/mbedtls/library').glob('*.c'))]
    add('USER_COMPILE_SOURCES',sources)
    path.write_text(text)
