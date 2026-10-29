# User-reported board results

Before the archive.org repair, the user reported that downloading a ProDOS
file from appletini.org worked perfectly. The archive.org failure concerned
its homepage, rather than a download URL.

After firmware candidate `b59c5f2fe8eb77d987a7bdc84c3bd6d94fe27bd5c748a467109c06e24c0b6a6f`
and matching browser raw binary
`d0bdef52f5e2608c4aa61830e1650dd94857635a14050e41e327fd0c009ff471`
were supplied, the user reported that it worked and requested F1.3.4 release
preparation. These are user reports. The installed flash hash was not
independently read, and no new board operation was performed by the release
builder.

F1.3.4 changes only `ps_sources/image_versions.h` from the working firmware
candidate. The versioned F1.3.4 image passed the binary/package audit; it has
not had a separate physical-board run. Native TLS results, software-emulated
browser tests and the reused FPGA timing report remain separate evidence.
