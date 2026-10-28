#ifndef HTTP_SERVICE_MBEDTLS_CONFIG_H
#define HTTP_SERVICE_MBEDTLS_CONFIG_H
#include "https_probe_mbedtls_config.h"
/* SHA-384/SHA-512 contexts contain uint64_t fields. Cortex-A9 faults on
 * unaligned doubleword accesses; the buffer allocator defaults to four. */
#define MBEDTLS_MEMORY_ALIGN_MULTIPLE 8
/* Internet chains include RSA roots and SHA-384/P-384 intermediates. */
#undef MBEDTLS_MPI_MAX_SIZE
#define MBEDTLS_MPI_MAX_SIZE 512
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA384_C
/* Parse old trust-anchor self-signatures; the default X.509 profile still
 * rejects SHA-1 signatures on the server/intermediate chain. */
#define MBEDTLS_SHA1_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
/* The pinned client patch yields during RSA-authenticated ECDHE too. */
#define APPLETINI_TLS_RSA_RESTART
#define MBEDTLS_PEM_PARSE_C
#endif
