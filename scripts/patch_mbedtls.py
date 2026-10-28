"""Apply the small, version-pinned Appletini scheduling patch."""
from pathlib import Path

OLD = '''    if (suite_info->key_exchange == MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA &&
        ssl->tls_version == MBEDTLS_SSL_VERSION_TLS1_2) {'''
NEW = '''    /* Appletini: the existing ECDH restart state machine also serves RSA
     * authenticated ECDHE. RSA signature checks remain synchronous; certificate
     * and hostname verification policy is unchanged. */
    if ((suite_info->key_exchange == MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA
#if defined(APPLETINI_TLS_RSA_RESTART)
         || suite_info->key_exchange == MBEDTLS_KEY_EXCHANGE_ECDHE_RSA
#endif
         ) && ssl->tls_version == MBEDTLS_SSL_VERSION_TLS1_2) {'''

def apply_patch(directory):
    path=Path(directory)/'library/ssl_tls12_client.c'
    source=path.read_text()
    if NEW in source:return
    if source.count(OLD)!=1:raise RuntimeError('Unexpected pinned TLS client source')
    path.write_text(source.replace(OLD,NEW),newline='\n')

if __name__=='__main__':
    apply_patch(Path(__file__).resolve().parents[1]/'third_party/mbedtls')
    print('Applied ECDHE-RSA restart scheduling patch; verification remains required.')
