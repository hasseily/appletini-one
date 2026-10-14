/* SPDX-License-Identifier: GPL-3.0-only */
#include "multiboot.h"
#include "crc32.h"
#include <string.h>
#include <limits.h>

#define IO_SIZE 4096U
#define BOOT_IDENT_OFFSET 0x24U
#define BOOT_IDENT 0x584C4E58U
#define META_VERSION 1U
#define META_CRC_OFFSET 124U
#define LEGACY_METADATA_SIZE 168U
#define LEGACY_CAPACITY 0x00DF0000U

typedef struct { const mb_flash_t *flash; uint32_t base; } flash_reader_t;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v)
{
    unsigned i;
    for (i=0; i<4; ++i) p[i]=(uint8_t)(v >> (8U*i));
}
static uint32_t crc(const void *p, uint32_t len)
{
    return crc32_finish(crc32_update(crc32_init(), p, len));
}
static int range(uint32_t off, uint32_t len, uint32_t size)
{
    return off <= size && len <= size-off;
}
static int product_valid(uint32_t product)
{
    return product==MB_PRODUCT_APPLETINI || product==MB_PRODUCT_MULTITINI;
}
static unsigned product_index(uint32_t product)
{
    return product==MB_PRODUCT_MULTITINI ? 1U : 0U;
}
uint32_t mb_product_offset(uint32_t product)
{
    return product==MB_PRODUCT_APPLETINI ? MB_APPLETINI_OFFSET :
           product==MB_PRODUCT_MULTITINI ? MB_MULTITINI_OFFSET : UINT32_MAX;
}
uint32_t mb_product_capacity(uint32_t product)
{
    return product==MB_PRODUCT_APPLETINI ? MB_APPLETINI_CAPACITY :
           product==MB_PRODUCT_MULTITINI ? MB_MULTITINI_CAPACITY : 0U;
}
const char *mb_product_name(uint32_t product)
{
    return product==MB_PRODUCT_APPLETINI ? "Appletini" :
           product==MB_PRODUCT_MULTITINI ? "Multitini" : "Legacy firmware";
}
static int flash_valid(const mb_flash_t *f, int writable)
{
    return f && f->read && f->capacity==MB_FLASH_SIZE &&
           (!writable || (f->erase && f->program));
}
static int read_at(const mb_reader_t *r, uint32_t off, void *dst, uint32_t len)
{
    if (!r || !r->read || !range(off,len,r->size)) return MB_E_READ;
    return r->read(r->context,off,dst,len)==0 ? MB_OK : MB_E_READ;
}
static int flash_read(void *context, uint32_t off, void *dst, uint32_t len)
{
    flash_reader_t *r=(flash_reader_t *)context;
    if (!range(r->base,off,r->flash->capacity) ||
        !range(r->base+off,len,r->flash->capacity)) return MB_E_READ;
    return r->flash->read(r->flash->context,r->base+off,dst,len);
}
static mb_reader_t slot_reader(const mb_flash_t *f, uint32_t off,
                               uint32_t size, flash_reader_t *context)
{
    mb_reader_t r;
    context->flash=f; context->base=off;
    r.context=context; r.read=flash_read; r.size=size;
    return r;
}
static int checksum_valid(const uint8_t *p, unsigned words)
{
    uint32_t sum=0; unsigned i;
    for (i=0;i<words;++i) sum+=le32(p+4U*i);
    return ~sum==le32(p+4U*words);
}
static int boot_header(const mb_reader_t *r, uint32_t off)
{
    uint8_t h[0x4c];
    if (read_at(r,off,h,sizeof h)!=MB_OK) return -1;
    return le32(h+0x20)==0xAA995566U && le32(h+0x24)==BOOT_IDENT &&
           checksum_valid(h+0x20,10);
}

/* Validate the same Bootgen partition and sentinel structure used by the
 * golden self-update engine. Every multiply/add is bounded before use. */
static int bootgen_validate(const mb_reader_t *r, uint32_t *payload_size)
{
    uint8_t h[0xa0], iht[64], pht[64];
    uint32_t io,po,count,end=0,first_start=0,first_total=0,first_load=0,first_exec=0;
    uint32_t i;
    int header;
    if (read_at(r,0,h,sizeof h)!=MB_OK) return MB_E_READ;
    header=boot_header(r,0);
    if (header<0) return MB_E_READ;
    if (!header) return MB_E_IMAGE;
    io=le32(h+0x98); po=le32(h+0x9c);
    if ((io&3U) || (po&3U) || !range(io,64,r->size) || !range(po,64,r->size)) return MB_E_IMAGE;
    if (read_at(r,io,iht,64)!=MB_OK) return MB_E_READ;
    count=le32(iht+4);
    if (!le32(iht) || le32(iht)==UINT32_MAX || !count || count>14 ||
        le32(iht+8)>UINT32_MAX/4U || le32(iht+8)*4U!=po ||
        le32(iht+12)>UINT32_MAX/4U || !range(le32(iht+12)*4U,64,r->size) ||
        (le32(iht+16) && (le32(iht+16)>UINT32_MAX/4U ||
         !range(le32(iht+16)*4U,4,r->size))) || count>(r->size-po)/64U)
        return MB_E_IMAGE;
    for (i=0;i<count;++i) {
        uint32_t total,data,start,check,attr;
        if (read_at(r,po+i*64U,pht,64)!=MB_OK) return MB_E_READ;
        if (!checksum_valid(pht,15)) return MB_E_IMAGE;
        total=le32(pht+8); data=le32(pht+4); start=le32(pht+20);
        attr=le32(pht+24); check=le32(pht+32);
        if (!total || !data || data>total || total>UINT32_MAX/4U || start>UINT32_MAX/4U ||
            !range(start*4U,total*4U,r->size) ||
            ((attr&0xf0U)!=0x10U && (attr&0xf0U)!=0x20U) ||
            (check && (check>UINT32_MAX/4U || !range(check*4U,4,r->size)))) return MB_E_IMAGE;
        if (start*4U+total*4U>end) end=start*4U+total*4U;
        if (!i) { first_start=start*4U; first_total=total*4U;
                  first_load=le32(pht+12); first_exec=le32(pht+16); }
    }
    if (!range(po+count*64U,64,r->size)) return MB_E_IMAGE;
    if (read_at(r,po+count*64U,pht,64)!=MB_OK) return MB_E_READ;
    for (i=0;i<15;++i) if (le32(pht+i*4U)) return MB_E_IMAGE;
    if (le32(pht+60)!=UINT32_MAX || first_start!=le32(h+0x30) ||
        first_total!=le32(h+0x40) || le32(h+0x34)>first_total ||
        first_load!=le32(h+0x38) || first_exec!=le32(h+0x3c) || !end || end>r->size)
        return MB_E_IMAGE;
    *payload_size=end;
    return MB_OK;
}

static uint32_t marker_step(const char *marker, uint32_t matched, uint8_t byte)
{
    while (matched && byte!=(uint8_t)marker[matched]) {
        uint32_t candidate=matched-1U;
        while (candidate && memcmp(marker,marker+matched-candidate,candidate)) --candidate;
        matched=candidate;
    }
    if (byte==(uint8_t)marker[matched]) ++matched;
    return matched;
}
/* Expected markers must not count as linked runtime identity themselves.
 * Read encoded reference bytes through volatile pointers so optimization,
 * including LTO, cannot place complete capability strings in this module.
 * Actual runtime/golden entry points link only their own plaintext identity.
 * Encoding distinguishes the reference table from identity; it is not image
 * authentication. The fixed manifest/CRC checks protect integrity separately. */
static const uint8_t marker_appletini[] = {
    0xE4,0xF5,0xF5,0xE9,0xE0,0xF1,0xEC,0xEB,0xEC,0x88,0xE8,
    0xF0,0xE9,0xF1,0xEC,0xE7,0xEA,0xEA,0xF1,0x88,0xF3,0x94
};
static const uint8_t marker_multitini[] = {
    0xE8,0xF0,0xE9,0xF1,0xEC,0xF1,0xEC,0xEB,0xEC,0x88,0xE8,
    0xF0,0xE9,0xF1,0xEC,0xE7,0xEA,0xEA,0xF1,0x88,0xF3,0x94
};
static const uint8_t marker_recovery[] = {
    0xE4,0xF5,0xF5,0xE9,0xE0,0xF1,0xEC,0xEB,0xEC,0x88,0xE2,0xEA,0xE9,0xE1,
    0xE0,0xEB,0x88,0xF7,0xE0,0xE6,0xEA,0xF3,0xE0,0xF7,0xFC,0x88,0xF3,0x94
};
static const uint8_t marker_golden[] = {
    0xE4,0xF5,0xF5,0xE9,0xE0,0xF1,0xEC,0xEB,0xEC,0x88,0xE8,0xF0,0xE9,0xF1,0xEC,
    0xE7,0xEA,0xEA,0xF1,0x88,0xE2,0xEA,0xE9,0xE1,0xE0,0xEB,0x88,0xF3,0x94
};
static void marker_decode(char *out, const uint8_t *encoded, uint32_t len)
{
    const volatile uint8_t *reference=encoded;
    uint32_t i;
    for (i=0;i<len;++i) out[i]=(char)(reference[i]^0xA5U);
    out[len]='\0';
}
/* Retain marker match state across reads, including overlapping prefixes. */
static int stream_validate(const mb_reader_t *r, uint32_t payload_size,
                           const char *marker, const char *opposite, uint32_t *payload_crc,
                           uint32_t *image_crc)
{
    uint8_t buf[IO_SIZE], footer[MB_MANIFEST_SIZE];
    char recovery[sizeof marker_recovery+1U];
    uint32_t offset=0,state=crc32_init(),match=0,recovery_match=0,opposite_match=0;
    uint32_t marker_len=marker ? (uint32_t)strlen(marker) : 0;
    uint32_t recovery_len=sizeof marker_recovery;
    uint32_t opposite_len=opposite ? (uint32_t)strlen(opposite) : 0;
    int found=!marker, recovery_found=0,opposite_found=0;
    marker_decode(recovery,marker_recovery,sizeof marker_recovery);
    while (offset<payload_size) {
        uint32_t chunk=payload_size-offset,i;
        if (chunk>IO_SIZE) chunk=IO_SIZE;
        if (read_at(r,offset,buf,chunk)!=MB_OK) return MB_E_READ;
        state=crc32_update(state,buf,chunk);
        for (i=0;i<chunk;++i) {
            uint8_t b=buf[i];
            if (!found) {
                match=marker_step(marker,match,b);
                if (match==marker_len) found=1;
            }
            if (!recovery_found) {
                recovery_match=marker_step(recovery,recovery_match,b);
                if (recovery_match==recovery_len) recovery_found=1;
            }
            if (opposite && !opposite_found) {
                opposite_match=marker_step(opposite,opposite_match,b);
                if (opposite_match==opposite_len) opposite_found=1;
            }
        }
        offset+=chunk;
    }
    *payload_crc=crc32_finish(state);
    if (read_at(r,payload_size,footer,sizeof footer)!=MB_OK) return MB_E_READ;
    *image_crc=crc32_finish(crc32_update(state,footer,sizeof footer));
    return found && recovery_found && !opposite_found ? MB_OK : MB_E_MARKER;
}
static int image_validate(const mb_reader_t *r, uint32_t product, int allow_legacy,
                          uint32_t role, int golden_marker, mb_image_t *out)
{
    uint8_t footer[MB_MANIFEST_SIZE];
    char expected_marker[sizeof marker_golden+1U],opposite_marker[sizeof marker_appletini+1U];
    uint32_t size,version,flags,payload_crc,image_crc,off;
    const char *marker=NULL;
    const char *opposite=NULL;
    int rc;
    if (!r || !out || !r->read || r->size<MB_MANIFEST_SIZE || r->size>LEGACY_CAPACITY ||
        (product && !product_valid(product))) return MB_E_ARGUMENT;
    memset(out,0,sizeof *out);
    rc=bootgen_validate(r,&size);
    if (rc!=MB_OK) return rc;
    if (size!=r->size-MB_MANIFEST_SIZE) return MB_E_IMAGE;
    if (read_at(r,size,footer,sizeof footer)!=MB_OK) return MB_E_READ;
    version=le32(footer+8); flags=le32(footer+16);
    if (memcmp(footer,"ATNIMG1\0",8) || le32(footer+12)!=role ||
        le32(footer+20)!=size || le32(footer+28)!=crc(footer,28)) return MB_E_MANIFEST;
    if (version==1U) {
        if (!allow_legacy || flags!=MB_FLAG_RECOVERY || product) return MB_E_MANIFEST;
    } else if (version==2U) {
        uint32_t tagged=flags&MB_PRODUCT_MASK;
        if (role!=2U || !product_valid(tagged) || (product && product!=tagged) ||
            flags!=(tagged|MB_FLAG_RECOVERY|MB_FLAG_MULTIBOOT)) return MB_E_MANIFEST;
        product=tagged;
        marker_decode(expected_marker,product==MB_PRODUCT_APPLETINI ? marker_appletini : marker_multitini,
                      sizeof marker_appletini);
        marker_decode(opposite_marker,product==MB_PRODUCT_APPLETINI ? marker_multitini : marker_appletini,
                      sizeof marker_appletini);
        marker=expected_marker; opposite=opposite_marker;
    } else return MB_E_MANIFEST;
    if ((role==1U && r->size>MB_GOLDEN_SIZE) ||
        (product && r->size>mb_product_capacity(product))) return MB_E_IMAGE;
    if (golden_marker) {
        marker_decode(expected_marker,marker_golden,sizeof marker_golden);
        marker=expected_marker;
    }
    /* Reject embedded BootROM candidates: partial installs must not expose a
     * valid secondary header before the primary XLNX word is committed. */
    for (off=0x8000U; range(off,0x4cU,size); off+=0x8000U) {
        int candidate=boot_header(r,off);
        if (candidate<0) return MB_E_READ;
        if (candidate) return MB_E_IMAGE;
    }
    rc=stream_validate(r,size,marker,opposite,&payload_crc,&image_crc);
    if (rc!=MB_OK) return rc;
    if (payload_crc!=le32(footer+24)) return MB_E_CRC;
    out->product=product; out->version=version; out->payload_size=size;
    out->payload_crc32=payload_crc; out->image_size=r->size; out->image_crc32=image_crc;
    return MB_OK;
}
int mb_image_validate(const mb_reader_t *reader, uint32_t expected_product,
                      int allow_legacy, mb_image_t *out)
{
    return image_validate(reader,expected_product,allow_legacy,2U,0,out);
}

/* Historical Appletini has no product tag. Its existing UART identity binds a
 * supported v1 archive to Appletini without changing its bytes or pretending
 * that it implements the multiboot interface. Encode references as above so
 * linking this validator into another runtime cannot confer that identity. */
static const uint8_t legacy_version_prefix[] = {
    0xA5,0xE3,0xCC,0xD7,0xC8,0xD2,0xC4,0xD7,0xC0,0x85,0xCC,0xC8,0xC4,
    0xC2,0xC0,0x9F,0x85,0xE3,0xCC,0xD7,0xC8,0xD2,0xC4,0xD7,0xC0,0x85,0xE3
};
static const uint8_t legacy_uart_prefix[] = {
    0xA8,0xAF,0xE4,0xD5,0xD5,0xC9,0xC0,0xD1,0xCC,0xCB,0xCC,0x85,0xC6,
    0xCA,0xCB,0xD1,0xD7,0xCA,0xC9,0x85,0xF0,0xE4,0xF7,0xF1,0xA8,0xAF
};
#define LEGACY_VERSION_SIZE 64U
#define FRONTEND_EXEC_ADDRESS 0x00100000U

static int legacy_version_supported(const char *text)
{
    const char *p=text;
    uint32_t parts[3],i;
    for (i=0;i<3U;++i) {
        uint32_t value=0;
        if (*p<'0' || *p>'9') return 0;
        do {
            uint32_t digit=(uint32_t)(*p-'0');
            if (value>(UINT32_MAX-digit)/10U) return 0;
            value=value*10U+digit; ++p;
        } while (*p>='0' && *p<='9');
        parts[i]=value;
        if (i<2U && *p++!='.') return 0;
    }
    if (*p=='-') {
        const char *suffix=++p;
        while ((*p>='0' && *p<='9') || (*p>='A' && *p<='Z') ||
               (*p>='a' && *p<='z') || *p=='.' || *p=='-') ++p;
        if (p==suffix) return 0;
    }
    if (*p++!='\r' || *p++!='\n' || *p) return 0;
    return parts[0]>1U || (parts[0]==1U && parts[1]>=1U);
}

/* Called only after Bootgen bounds and the complete v1 CRC have passed. Scan
 * actual CPU0 frontend load segments after its FPGA partition, never FSBL,
 * partition tables, bitstream, padding or footer. All F1.1.0+ release archives
 * use this entry address and these startup identities. */
static int legacy_appletini_identity(const mb_reader_t *r)
{
    uint8_t h[0xa0],iht[64],pht[64],buf[IO_SIZE];
    char prefix[sizeof legacy_version_prefix+1U],uart[sizeof legacy_uart_prefix+1U];
    char recovery[sizeof marker_recovery+1U],opposite[sizeof marker_multitini+1U];
    char golden[sizeof marker_golden+1U],version[LEGACY_VERSION_SIZE]={0},candidate[LEGACY_VERSION_SIZE];
    uint32_t po,count,i,previous_end=FRONTEND_EXEC_ADDRESS;
    int executed=0,pl_found=0,found=0,uart_found=0,recovery_found=0;
    marker_decode(prefix,legacy_version_prefix,sizeof legacy_version_prefix);
    marker_decode(uart,legacy_uart_prefix,sizeof legacy_uart_prefix);
    marker_decode(recovery,marker_recovery,sizeof marker_recovery);
    marker_decode(opposite,marker_multitini,sizeof marker_multitini);
    marker_decode(golden,marker_golden,sizeof marker_golden);
    if (read_at(r,0,h,sizeof h)!=MB_OK || read_at(r,le32(h+0x98),iht,64)!=MB_OK)
        return MB_E_READ;
    po=le32(h+0x9c); count=le32(iht+4);
    if (count<3U) return MB_E_IMAGE;
    for (i=1;i<count;++i) {
        uint32_t load,exec,start,size,off,attr;
        uint32_t vm=0,um=0,rm=0,om=0,gm=0,length=0;
        int collecting=0;
        if (read_at(r,po+i*64U,pht,sizeof pht)!=MB_OK) return MB_E_READ;
        load=le32(pht+12); exec=le32(pht+16); attr=le32(pht+24)&0xf0U;
        start=le32(pht+20)*4U; size=le32(pht+4)*4U;
        if (attr==0x20U) {
            if (pl_found || executed || load || exec) return MB_E_IMAGE;
            pl_found=1; continue;
        }
        if (attr!=0x10U || !pl_found || load<previous_end || size>UINT32_MAX-load ||
            (!executed && (load!=FRONTEND_EXEC_ADDRESS || exec!=FRONTEND_EXEC_ADDRESS)) ||
            (executed && exec)) return MB_E_IMAGE;
        executed=1; previous_end=load+size;
        for (off=0;off<size;) {
            uint32_t n=size-off,j;
            if (n>sizeof buf) n=sizeof buf;
            if (read_at(r,start+off,buf,n)!=MB_OK) return MB_E_READ;
            for (j=0;j<n;++j) {
                uint8_t byte=buf[j];
                if (!uart_found) {
                    um=marker_step(uart,um,byte);
                    if (um==sizeof legacy_uart_prefix) uart_found=1;
                }
                if (!recovery_found) {
                    rm=marker_step(recovery,rm,byte);
                    if (rm==sizeof marker_recovery) recovery_found=1;
                }
                om=marker_step(opposite,om,byte); gm=marker_step(golden,gm,byte);
                if (om==sizeof marker_multitini || gm==sizeof marker_golden) return MB_E_MARKER;
                if (collecting) {
                    if (length>=sizeof candidate-1U) return MB_E_MARKER;
                    candidate[length++]=(char)byte;
                    if (!byte) {
                        if (!legacy_version_supported(candidate) ||
                            (found && strcmp(version,candidate))) return MB_E_MARKER;
                        memcpy(version,candidate,length); found=1;
                        collecting=0; vm=0;
                    }
                } else {
                    vm=marker_step(prefix,vm,byte);
                    if (vm==sizeof legacy_version_prefix) { collecting=1; length=0; vm=0; }
                }
            }
            off+=n;
        }
        if (collecting) return MB_E_MARKER;
    }
    return executed && found && uart_found && recovery_found ? MB_OK : MB_E_MARKER;
}

int mb_firmware_validate(const mb_reader_t *reader, mb_image_t *out)
{
    int rc=mb_image_validate(reader,0,0,out);
    if (rc!=MB_E_MANIFEST) return rc;
    rc=mb_image_validate(reader,0,1,out);
    if (rc!=MB_OK) return rc;
    if (out->version!=1U || out->image_size>MB_APPLETINI_CAPACITY) return MB_E_IMAGE;
    rc=legacy_appletini_identity(reader);
    if (rc==MB_OK) out->product=MB_PRODUCT_APPLETINI;
    return rc;
}
static int flash_image(const mb_flash_t *f, uint32_t offset, uint32_t capacity,
                       uint32_t product, int legacy, uint32_t role, int golden,
                       mb_image_t *out)
{
    flash_reader_t context;
    mb_reader_t r=slot_reader(f,offset,capacity,&context);
    uint32_t payload_size;
    int rc=bootgen_validate(&r,&payload_size);
    if (rc!=MB_OK) return rc;
    if (!range(payload_size,MB_MANIFEST_SIZE,capacity)) return MB_E_IMAGE;
    r.size=payload_size+MB_MANIFEST_SIZE;
    return image_validate(&r,product,legacy,role,golden,out);
}
static void metadata_init(mb_metadata_t *m)
{
    unsigned i;
    memset(m,0,sizeof *m); m->sector_offset=UINT32_MAX;
    for (i=0;i<2;++i) {
        uint32_t p=i ? MB_PRODUCT_MULTITINI : MB_PRODUCT_APPLETINI;
        m->slots[i].product=p; m->slots[i].offset=mb_product_offset(p);
        m->slots[i].capacity=mb_product_capacity(p);
    }
}
static int metadata_decode(const uint8_t b[MB_METADATA_SIZE], uint32_t sector,
                            mb_metadata_t *m)
{
    unsigned i,j;
    if (memcmp(b,"ATNMBT1\0",8) || le32(b+8)!=META_VERSION ||
        le32(b+12)!=MB_METADATA_SIZE || !le32(b+16) ||
        le32(b+META_CRC_OFFSET)!=crc(b,META_CRC_OFFSET) ||
        (le32(b+20) && !product_valid(le32(b+20))) ||
        (le32(b+88) && !product_valid(le32(b+88)))) return MB_E_METADATA;
    metadata_init(m); m->sequence=le32(b+16); m->default_product=le32(b+20);
    m->request_product=le32(b+88);
    m->sector_offset=sector;
    for (i=0;i<2;++i) {
        mb_slot_record_t *s=&m->slots[i]; const uint8_t *p=b+24U+32U*i;
        s->product=le32(p); s->offset=le32(p+4); s->capacity=le32(p+8);
        s->payload_size=le32(p+12); s->payload_crc32=le32(p+16);
        s->image_size=le32(p+20); s->image_crc32=le32(p+24); s->flags=le32(p+28);
        if (s->offset!=(i ? MB_MULTITINI_OFFSET : MB_APPLETINI_OFFSET) ||
            s->capacity!=(i ? MB_MULTITINI_CAPACITY : MB_APPLETINI_CAPACITY) ||
            (s->flags&~(MB_FLAG_SLOT_VALID|MB_FLAG_SLOT_LEGACY)) ||
            (s->product!=(i ? MB_PRODUCT_MULTITINI : MB_PRODUCT_APPLETINI) &&
             (i || s->product!=MB_PRODUCT_NONE || s->flags!=(MB_FLAG_SLOT_VALID|MB_FLAG_SLOT_LEGACY))) ||
            ((s->flags&MB_FLAG_SLOT_LEGACY) && (i || s->product!=MB_PRODUCT_NONE)) ||
            ((s->flags&MB_FLAG_SLOT_VALID) &&
             (s->payload_size<0xa0U || s->payload_size>s->capacity-MB_MANIFEST_SIZE ||
              s->image_size!=s->payload_size+MB_MANIFEST_SIZE))) return MB_E_METADATA;
    }
    for (j=92U;j<META_CRC_OFFSET;++j) if (b[j]) return MB_E_METADATA;
    return MB_OK;
}
static void metadata_encode(const mb_metadata_t *m, uint8_t b[MB_METADATA_SIZE])
{
    unsigned i;
    memset(b,0,MB_METADATA_SIZE); memcpy(b,"ATNMBT1\0",8);
    put32(b+8,META_VERSION); put32(b+12,MB_METADATA_SIZE);
    put32(b+16,m->sequence); put32(b+20,m->default_product);
    put32(b+88,m->request_product);
    for (i=0;i<2;++i) {
        const mb_slot_record_t *s=&m->slots[i]; uint8_t *p=b+24U+32U*i;
        put32(p,s->product); put32(p+4,s->offset); put32(p+8,s->capacity);
        put32(p+12,s->payload_size); put32(p+16,s->payload_crc32);
        put32(p+20,s->image_size); put32(p+24,s->image_crc32); put32(p+28,s->flags);
    }
    put32(b+META_CRC_OFFSET,crc(b,META_CRC_OFFSET));
}
int mb_metadata_load(const mb_flash_t *f, mb_metadata_t *out)
{
    uint8_t a[MB_METADATA_SIZE],b[MB_METADATA_SIZE];
    mb_metadata_t ma,mb; int va,vb;
    if (!flash_valid(f,0) || !out) return MB_E_ARGUMENT;
    metadata_init(out);
    /* A failed read might hide a newer invalidation; never trust the other
     * sector alone in that case. CRC-invalid/erased sectors are safe to ignore. */
    if (f->read(f->context,MB_METADATA_A_OFFSET,a,sizeof a) ||
        f->read(f->context,MB_METADATA_B_OFFSET,b,sizeof b)) return MB_E_READ;
    va=metadata_decode(a,MB_METADATA_A_OFFSET,&ma)==MB_OK;
    vb=metadata_decode(b,MB_METADATA_B_OFFSET,&mb)==MB_OK;
    if (!va && !vb) return MB_E_METADATA;
    if (va && vb) {
        uint32_t delta=ma.sequence-mb.sequence;
        if (!delta) {
            if (memcmp(a,b,sizeof a)) return MB_E_METADATA;
            *out=ma;
        } else if (delta==0x80000000U) return MB_E_METADATA;
        else *out=delta<0x80000000U ? ma : mb;
    } else *out=va ? ma : mb;
    return MB_OK;
}
static int metadata_commit(const mb_flash_t *f, mb_metadata_t *m)
{
    uint8_t b[MB_METADATA_SIZE],verify[MB_METADATA_SIZE];
    mb_metadata_t decoded;
    uint32_t destination=m->sector_offset==MB_METADATA_A_OFFSET ?
                         MB_METADATA_B_OFFSET : MB_METADATA_A_OFFSET;
    ++m->sequence; if (!m->sequence) m->sequence=1;
    metadata_encode(m,b);
    if (metadata_decode(b,destination,&decoded)!=MB_OK) return MB_E_METADATA;
    if (f->erase(f->context,destination,MB_SECTOR_SIZE)) return MB_E_ERASE;
    if (f->program(f->context,destination,b,META_CRC_OFFSET) ||
        f->program(f->context,destination+META_CRC_OFFSET,b+META_CRC_OFFSET,4)) return MB_E_PROGRAM;
    if (f->read(f->context,destination,verify,sizeof verify)) return MB_E_READ;
    if (memcmp(b,verify,sizeof b) || metadata_decode(verify,destination,&decoded)!=MB_OK)
        return MB_E_VERIFY;
    *m=decoded;
    return MB_OK;
}
static int image_matches(const mb_slot_record_t *s, const mb_image_t *image)
{
    return s->product==image->product && s->payload_size==image->payload_size &&
           s->payload_crc32==image->payload_crc32 && s->image_size==image->image_size &&
           s->image_crc32==image->image_crc32;
}
static int recorded_slot_verify(const mb_flash_t *f, const mb_slot_record_t *s,
                                int legacy, mb_image_t *out)
{
    int rc;
    if (!(s->flags&MB_FLAG_SLOT_VALID) ||
        !!(s->flags&MB_FLAG_SLOT_LEGACY)!=!!legacy) return MB_E_UNAVAILABLE;
    rc=flash_image(f,s->offset,s->capacity,s->product,legacy,2U,0,out);
    if (rc!=MB_OK) return rc;
    return image_matches(s,out) ? MB_OK : MB_E_VERIFY;
}
static int identify_legacy_appletini(const mb_flash_t *f, uint32_t offset, mb_image_t *image)
{
    flash_reader_t context;
    mb_reader_t reader;
    int rc;
    if (image->version!=1U || image->product!=MB_PRODUCT_NONE ||
        image->image_size>MB_APPLETINI_CAPACITY) return MB_E_UNAVAILABLE;
    reader=slot_reader(f,offset,image->image_size,&context);
    rc=legacy_appletini_identity(&reader);
    if (rc==MB_OK) image->product=MB_PRODUCT_APPLETINI;
    return rc;
}
static int recorded_appletini_verify(const mb_flash_t *f, const mb_slot_record_t *s,
                                     mb_image_t *out)
{
    int rc=recorded_slot_verify(f,s,1,out);
    return rc==MB_OK ? identify_legacy_appletini(f,s->offset,out) : rc;
}
static int old_legacy_verify(const mb_flash_t *f, mb_image_t *out);
int mb_slot_verify(const mb_flash_t *f, uint32_t product, mb_image_t *out)
{
    mb_metadata_t m; mb_image_t local; int rc;
    if (!product_valid(product)) return MB_E_ARGUMENT;
    if (!out) out=&local;
    memset(out,0,sizeof *out);
    rc=mb_metadata_load(f,&m);
    if (rc==MB_E_METADATA) {
        rc=flash_image(f,mb_product_offset(product),mb_product_capacity(product),product,0,2U,0,out);
        if (rc!=MB_OK && rc!=MB_E_READ && product==MB_PRODUCT_APPLETINI) {
            rc=old_legacy_verify(f,out);
            if (rc==MB_OK) rc=identify_legacy_appletini(f,MB_APPLETINI_OFFSET,out);
        }
        return rc;
    }
    if (rc!=MB_OK) return rc;
    if (m.slots[product_index(product)].product!=product) {
        if (product==MB_PRODUCT_APPLETINI)
            return recorded_appletini_verify(f,&m.slots[0],out);
        return MB_E_UNAVAILABLE;
    }
    return recorded_slot_verify(f,&m.slots[product_index(product)],0,out);
}
int mb_slot_probe(const mb_flash_t *f, uint32_t product, mb_image_t *out)
{
    mb_metadata_t metadata;
    mb_image_t image;
    flash_reader_t context;
    mb_reader_t reader;
    uint8_t footer[MB_MANIFEST_SIZE];
    uint32_t size,capacity,flags;
    int rc;
    if (out) memset(out,0,sizeof *out);
    if (!flash_valid(f,0) || !product_valid(product)) return MB_E_ARGUMENT;
    /* A committed invalidation wins over a still-intact old payload. A read
     * failure could hide a newer invalidation and must fail closed. */
    rc=mb_metadata_load(f,&metadata);
    if (rc!=MB_OK && rc!=MB_E_METADATA) return rc;
    if (rc==MB_OK) {
        const mb_slot_record_t *slot=&metadata.slots[product_index(product)];
        if (product==MB_PRODUCT_APPLETINI &&
            slot->flags==(MB_FLAG_SLOT_VALID|MB_FLAG_SLOT_LEGACY)) {
            /* Legacy metadata intentionally retains unknown product for old
             * runtimes. Verify its actual loaded identity before presenting A. */
            rc=recorded_appletini_verify(f,slot,&image);
            if (rc==MB_OK && out) *out=image;
            return rc;
        }
        if (slot->product!=product || slot->flags!=MB_FLAG_SLOT_VALID)
            return MB_E_UNAVAILABLE;
    }
    capacity=mb_product_capacity(product);
    reader=slot_reader(f,mb_product_offset(product),capacity,&context);
    {
        int image_rc=bootgen_validate(&reader,&size);
        if (image_rc!=MB_OK) return image_rc;
    }
    if (!range(size,sizeof footer,capacity)) return MB_E_IMAGE;
    if (read_at(&reader,size,footer,sizeof footer)!=MB_OK) return MB_E_READ;
    if (rc==MB_E_METADATA && product==MB_PRODUCT_APPLETINI &&
        !memcmp(footer,"ATNIMG1\0",8) && le32(footer+8)==1U) {
        /* Pre-adoption compatibility still needs its guarded v4 record and
         * the full original image; an advisory footer alone cannot identify F. */
        return mb_slot_verify(f,product,out);
    }
    flags=product|MB_FLAG_RECOVERY|MB_FLAG_MULTIBOOT;
    if (memcmp(footer,"ATNIMG1\0",8) || le32(footer+8)!=2U ||
        le32(footer+12)!=2U || le32(footer+16)!=flags ||
        le32(footer+20)!=size || le32(footer+28)!=crc(footer,28))
        return MB_E_MANIFEST;
    memset(&image,0,sizeof image);
    image.product=product; image.version=2U; image.payload_size=size;
    image.payload_crc32=le32(footer+24); image.image_size=size+sizeof footer;
    if (rc==MB_OK) {
        const mb_slot_record_t *slot=&metadata.slots[product_index(product)];
        if (slot->payload_size!=image.payload_size ||
            slot->payload_crc32!=image.payload_crc32 ||
            slot->image_size!=image.image_size) return MB_E_VERIFY;
    }
    if (out) *out=image;
    return MB_OK;
}
static int old_legacy_verify(const mb_flash_t *f, mb_image_t *out)
{
    uint8_t b[LEGACY_METADATA_SIZE];
    int rc;
    if (f->read(f->context,MB_METADATA_B_OFFSET,b,sizeof b)) return MB_E_READ;
    if (le32(b)!=0x46574D54U || le32(b+4)!=4U || le32(b+8)!=sizeof b ||
        le32(b+16)!=0U || le32(b+20)!=0x00200000U ||
        le32(b+24)!=MB_APPLETINI_OFFSET || le32(b+28)!=LEGACY_CAPACITY ||
        le32(b+32)!=1U || le32(b+sizeof b-4)!=crc(b,sizeof b-4)) return MB_E_METADATA;
    rc=flash_image(f,MB_APPLETINI_OFFSET,LEGACY_CAPACITY,0,1,2U,0,out);
    if (rc==MB_OK && (out->version!=1U || out->product!=MB_PRODUCT_NONE)) return MB_E_MANIFEST;
    return rc;
}
static void record_image(mb_slot_record_t *s, const mb_image_t *image, int legacy)
{
    s->product=legacy ? MB_PRODUCT_NONE : image->product; s->payload_size=image->payload_size;
    s->payload_crc32=image->payload_crc32; s->image_size=image->image_size;
    s->image_crc32=image->image_crc32;
    s->flags=MB_FLAG_SLOT_VALID|(legacy ? MB_FLAG_SLOT_LEGACY : 0U);
}
int mb_select(const mb_flash_t *f, uint32_t requested, mb_selection_t *out)
{
    mb_metadata_t m; mb_image_t image; uint32_t first,other; int rc;
    if (!flash_valid(f,0) || !out || (requested && !product_valid(requested))) return MB_E_ARGUMENT;
    memset(out,0,sizeof *out); out->boot_offset=MB_GOLDEN_OFFSET;
    rc=mb_metadata_load(f,&m);
    if (rc==MB_E_READ) return rc;
    if (rc==MB_OK) {
        first=requested ? requested : (m.request_product ? m.request_product : m.default_product);
        if (!product_valid(first)) first=MB_PRODUCT_APPLETINI;
        other=first==MB_PRODUCT_APPLETINI ? MB_PRODUCT_MULTITINI : MB_PRODUCT_APPLETINI;
        {
            uint32_t order[2]={first,other}; unsigned i;
            for (i=0;i<2U;++i) {
                uint32_t p=order[i]; const mb_slot_record_t *s=&m.slots[product_index(p)];
                int legacy=(p==MB_PRODUCT_APPLETINI && (s->flags&MB_FLAG_SLOT_LEGACY));
                rc=legacy ? recorded_appletini_verify(f,s,&image) :
                    s->product==p ? recorded_slot_verify(f,s,0,&image) : MB_E_UNAVAILABLE;
                if (rc==MB_OK) {
                    out->product=p; out->boot_offset=mb_product_offset(p);
                    out->legacy=(uint32_t)legacy; out->image=image; return MB_OK;
                }
            }
        }
        if (recorded_slot_verify(f,&m.slots[0],1,&image)==MB_OK) {
            out->boot_offset=MB_APPLETINI_OFFSET; out->legacy=1; out->image=image;
        }
    } else {
        first=requested ? requested : MB_PRODUCT_APPLETINI;
        other=first==MB_PRODUCT_APPLETINI ? MB_PRODUCT_MULTITINI : MB_PRODUCT_APPLETINI;
        {
            uint32_t order[2]={first,other}; unsigned i;
            for (i=0;i<2U;++i) {
                uint32_t p=order[i];
                rc=mb_slot_verify(f,p,&image);
                if (rc==MB_OK) {
                    out->product=p; out->boot_offset=mb_product_offset(p);
                    out->legacy=(uint32_t)(image.version==1U); out->image=image; return MB_OK;
                }
            }
        }
        if (old_legacy_verify(f,&image)==MB_OK) {
            out->boot_offset=MB_APPLETINI_OFFSET; out->legacy=1; out->image=image;
        }
    }
    return MB_OK;
}
int mb_golden_available(const mb_flash_t *f, uint32_t *offset_out)
{
    mb_image_t image;
    if (!flash_valid(f,0)) return MB_E_ARGUMENT;
    if (flash_image(f,MB_GOLDEN_OFFSET,MB_GOLDEN_SIZE,0,1,1U,1,&image)==MB_OK) {
        if (offset_out) *offset_out=MB_GOLDEN_OFFSET;
        return MB_OK;
    }
    if (flash_image(f,MB_GOLDEN_TRIAL_OFFSET,MB_GOLDEN_SIZE,0,1,1U,1,&image)==MB_OK) {
        if (offset_out) *offset_out=MB_GOLDEN_TRIAL_OFFSET;
        return MB_OK;
    }
    return MB_E_UNAVAILABLE;
}

#define GOLDEN_VERSION_SIZE 64U
#define GOLDEN_EXEC_ADDRESS 0x00100000U
/* These are UART identity labels, not version references. A version is accepted
 * only after the loaded Golden application's complete Bootgen/manifest checks. */
static const char golden_version_prefix[] = "[GOLDEN] Boot image version: ";
static const char golden_start_prefix[] = "[GOLDEN] boot_updater start @";

static int golden_version_parse(char *text, uint32_t *major, uint32_t *minor)
{
    char *p=text, *version;
    uint32_t components[3],i;
    size_t len=strlen(text);
    if (len<2U || text[len-2U]!='\r' || text[len-1U]!='\n') return 0;
    text[len-2U]='\0';
    if (!strncmp(p,"Boot ",5)) p+=5;
    else if (!strncmp(p,"Multiboot ",10)) p+=10;
    else return 0;
    version=p;
    if (*p++!='B') return 0;
    for (i=0;i<3U;++i) {
        uint32_t value=0;
        if (*p<'0' || *p>'9') return 0;
        do {
            uint32_t digit=(uint32_t)(*p-'0');
            if (value>(UINT32_MAX-digit)/10U) return 0;
            value=value*10U+digit; ++p;
        } while (*p>='0' && *p<='9');
        components[i]=value;
        if (i<2U && *p++!='.') return 0;
    }
    if (*p=='-') {
        ++p;
        if (!*p) return 0;
        while ((*p>='0' && *p<='9') || (*p>='A' && *p<='Z') ||
               (*p>='a' && *p<='z') || *p=='.' || *p=='-') ++p;
    }
    if (*p) return 0;
    *major=components[0]; *minor=components[1];
    memmove(text,version,strlen(version)+1U);
    return 1;
}
typedef struct {
    char version[GOLDEN_VERSION_SIZE],candidate[GOLDEN_VERSION_SIZE];
    uint32_t version_match,start_match,length,major,minor;
    int collecting,found,start_found,conflicting;
} golden_identity_t;

static void golden_identity_byte(golden_identity_t *s, uint8_t byte)
{
    if (!s->start_found) {
        s->start_match=marker_step(golden_start_prefix,s->start_match,byte);
        if (s->start_match==sizeof golden_start_prefix-1U) s->start_found=1;
    }
    if (s->collecting) {
        if (s->length>=sizeof s->candidate-1U) {
            s->collecting=0; s->version_match=0;
        } else {
            s->candidate[s->length++]=(char)byte;
            if (!byte) {
                uint32_t major,minor;
                if (golden_version_parse(s->candidate,&major,&minor)) {
                    if (s->found && strcmp(s->version,s->candidate)) s->conflicting=1;
                    else {
                        memcpy(s->version,s->candidate,strlen(s->candidate)+1U);
                        s->major=major; s->minor=minor; s->found=1;
                    }
                }
                s->collecting=0; s->version_match=0;
            }
        }
    } else {
        s->version_match=marker_step(golden_version_prefix,s->version_match,byte);
        if (s->version_match==sizeof golden_version_prefix-1U) {
            s->collecting=1; s->length=0; s->version_match=0;
        }
    }
}

/* Scan only executable/data bytes actually loaded for Golden at 1 MiB. Skip
 * FSBL, partition tables, file padding, unused flash and the manifest footer.
 * Golden consists of the FSBL plus one CPU0 ELF (possibly split into segments),
 * with no bitstream, other executable or overlapping application segments. */
static int golden_loaded_identity(const mb_reader_t *r, golden_identity_t *identity)
{
    uint8_t h[0xa0],iht[64],pht[64],buf[IO_SIZE];
    uint32_t po,count,i,previous_end=GOLDEN_EXEC_ADDRESS;
    int executed=0;
    memset(identity,0,sizeof *identity);
    if (read_at(r,0,h,sizeof h)!=MB_OK || read_at(r,le32(h+0x98),iht,64)!=MB_OK)
        return MB_E_READ;
    po=le32(h+0x9c); count=le32(iht+4);
    if (count<2U) return MB_E_IMAGE;
    for (i=1;i<count;++i) {
        uint32_t load,exec,start,size,off;
        if (read_at(r,po+i*64U,pht,sizeof pht)!=MB_OK) return MB_E_READ;
        load=le32(pht+12); exec=le32(pht+16);
        start=le32(pht+20)*4U; size=le32(pht+4)*4U;
        if ((le32(pht+24)&0xf0U)!=0x10U || load<previous_end ||
            size>UINT32_MAX-load ||
            (!executed && (load!=GOLDEN_EXEC_ADDRESS || exec!=GOLDEN_EXEC_ADDRESS)) ||
            (executed && exec)) return MB_E_IMAGE;
        previous_end=load+size; executed=1;
        /* Identity strings cannot span distinct ELF load segments. */
        identity->version_match=identity->start_match=identity->length=0;
        identity->collecting=0;
        for (off=0;off<size;) {
            uint32_t n=size-off,j;
            if (n>sizeof buf) n=sizeof buf;
            if (read_at(r,start+off,buf,n)!=MB_OK) return MB_E_READ;
            for (j=0;j<n;++j) golden_identity_byte(identity,buf[j]);
            off+=n;
        }
    }
    return identity->found && identity->start_found && !identity->conflicting ? MB_OK : MB_E_MARKER;
}
static int golden_installed_at(const mb_flash_t *f, uint32_t offset,
                               char version[GOLDEN_VERSION_SIZE], int *valid_copy)
{
    flash_reader_t context;
    mb_reader_t r=slot_reader(f,offset,MB_GOLDEN_SIZE,&context);
    mb_image_t image; golden_identity_t identity;
    uint8_t footer[MB_MANIFEST_SIZE];
    uint32_t payload_size,off;
    int rc=bootgen_validate(&r,&payload_size);
    *valid_copy=0;
    if (rc!=MB_OK) return rc;
    if (!range(payload_size,sizeof footer,MB_GOLDEN_SIZE)) return MB_E_IMAGE;
    if (read_at(&r,payload_size,footer,sizeof footer)!=MB_OK) return MB_E_READ;
    /* A valid footer provides full image integrity. A malformed footer never
     * downgrades a B1.2/B2 image to the historical raw-B1.1 path below. */
    if (!memcmp(footer,"ATNIMG1\0",8)) {
        r.size=payload_size+sizeof footer;
        rc=image_validate(&r,0,1,1U,0,&image);
        if (rc!=MB_OK) return rc;
    } else {
        for (off=0;off<sizeof footer;++off) if (footer[off]!=0xffU) return MB_E_MANIFEST;
        r.size=payload_size;
        for (off=0x8000U; range(off,0x4cU,payload_size); off+=0x8000U) {
            int candidate=boot_header(&r,off);
            if (candidate<0) return MB_E_READ;
            if (candidate) return MB_E_IMAGE;
        }
    }
    rc=golden_loaded_identity(&r,&identity);
    if (rc!=MB_OK) {
        /* Do not label an intact primary with the trial's version merely
         * because a future Golden changes its UART banner. */
        if (rc==MB_E_MARKER && (r.size!=payload_size || identity.start_found)) *valid_copy=1;
        return rc;
    }
    if (r.size==payload_size) {
        if (identity.major!=1U || identity.minor!=1U || strchr(identity.version,'-'))
            return MB_E_MANIFEST;
    } else if (identity.major>=2U) {
        rc=image_validate(&r,0,1,1U,1,&image);
        if (rc!=MB_OK) return rc;
    }
    *valid_copy=1;
    memcpy(version,identity.version,strlen(identity.version)+1U);
    return MB_OK;
}
int mb_golden_installed_version(const mb_flash_t *f, char *version,
                                uint32_t capacity, uint32_t *slot_offset)
{
    char discovered[GOLDEN_VERSION_SIZE];
    uint32_t offset;
    int rc,valid_copy;
    if (version && capacity) version[0]='\0';
    if (slot_offset) *slot_offset=UINT32_MAX;
    if (!flash_valid(f,0) || !version || !capacity) return MB_E_ARGUMENT;
    for (offset=MB_GOLDEN_OFFSET;offset<=MB_GOLDEN_TRIAL_OFFSET;offset+=MB_GOLDEN_TRIAL_OFFSET) {
        rc=golden_installed_at(f,offset,discovered,&valid_copy);
        if (rc==MB_OK) {
            if (strlen(discovered)>=capacity) return MB_E_ARGUMENT;
            memcpy(version,discovered,strlen(discovered)+1U);
            if (slot_offset) *slot_offset=offset;
            return MB_OK;
        }
        if (valid_copy) return MB_E_UNAVAILABLE;
    }
    return MB_E_UNAVAILABLE;
}
static int metadata_adopt(const mb_flash_t *f, mb_metadata_t *m,
                           const mb_image_t *checked)
{
    mb_image_t image; unsigned i; int rc=mb_metadata_load(f,m);
    if (rc!=MB_E_METADATA) return rc;
    metadata_init(m);
    for (i=0;i<2;++i) {
        uint32_t p=i ? MB_PRODUCT_MULTITINI : MB_PRODUCT_APPLETINI;
        /* Reuse only this transaction's private live target verification.
         * An external/cached image record must never authorize adoption. */
        if (checked && checked->product==p) { image=*checked; rc=MB_OK; }
        else rc=flash_image(f,mb_product_offset(p),mb_product_capacity(p),p,0,2U,0,&image);
        if (rc==MB_E_READ) return rc;
        if (rc==MB_OK) record_image(&m->slots[i],&image,0);
    }
    if (!(m->slots[0].flags&MB_FLAG_SLOT_VALID)) {
        rc=old_legacy_verify(f,&image);
        if (rc==MB_E_READ) return rc;
        if (rc==MB_OK && image.image_size<=MB_APPLETINI_CAPACITY) record_image(&m->slots[0],&image,1);
    }
    /* First migration must preserve the verified tagged boot fallback that
     * existed before an update started. An old Golden can install tagged
     * Appletini without creating multiboot metadata; adding Multitini later
     * must not make that newly installed peer the startup default. */
    if ((m->slots[0].flags&MB_FLAG_SLOT_VALID) &&
        (m->slots[0].product==MB_PRODUCT_APPLETINI ||
         recorded_appletini_verify(f,&m->slots[0],&image)==MB_OK))
        m->default_product=MB_PRODUCT_APPLETINI;
    else if ((m->slots[1].flags&MB_FLAG_SLOT_VALID) &&
             m->slots[1].product==MB_PRODUCT_MULTITINI)
        m->default_product=MB_PRODUCT_MULTITINI;
    return MB_OK;
}
static int set_choice(const mb_flash_t *f, uint32_t product, int request,
                       uint32_t *golden_offset)
{
    mb_metadata_t m; mb_image_t image; uint32_t offset; int rc;
    if (golden_offset) *golden_offset=UINT32_MAX;
    if (!flash_valid(f,1) || !product_valid(product)) return MB_E_ARGUMENT;
    if (mb_golden_available(f,&offset)!=MB_OK) return MB_E_UNAVAILABLE;
    if (request) {
        rc=mb_metadata_load(f,&m);
        if (rc==MB_OK) {
            /* Switching delegates full payload integrity to Golden after
             * reset. Committed eligibility and bounded live headers/footer
             * only authorize this request, never direct firmware execution. */
            rc=mb_slot_probe(f,product,NULL); if (rc!=MB_OK) return rc;
        } else if (rc==MB_E_METADATA) {
            /* First migration must establish records from full live checks,
             * rather than promote header-only presence into trusted state. */
            const mb_slot_record_t *slot;
            rc=metadata_adopt(f,&m,NULL); if (rc!=MB_OK) return rc;
            slot=&m.slots[product_index(product)];
            if (slot->product!=product || slot->flags!=MB_FLAG_SLOT_VALID)
                return MB_E_UNAVAILABLE;
        } else return rc;
    } else {
        /* A persistent startup choice still rejects a corrupt target now. */
        rc=mb_slot_verify(f,product,&image); if (rc!=MB_OK) return rc;
        rc=metadata_adopt(f,&m,&image); if (rc!=MB_OK) return rc;
    }
    if (request) {
        if (m.request_product==product && m.sector_offset!=UINT32_MAX) {
            if (golden_offset) *golden_offset=offset;
            return MB_OK;
        }
        m.request_product=product;
    } else {
        if (m.default_product==product && m.sector_offset!=UINT32_MAX) return MB_OK;
        m.default_product=product;
    }
    rc=metadata_commit(f,&m);
    if (rc==MB_OK && golden_offset) *golden_offset=offset;
    return rc;
}
int mb_set_default(const mb_flash_t *f, uint32_t product)
{
    return set_choice(f,product,0,NULL);
}
int mb_set_request(const mb_flash_t *f, uint32_t product)
{
    return set_choice(f,product,1,NULL);
}
int mb_prepare_request(const mb_flash_t *f, uint32_t product, uint32_t *golden_offset)
{
    if (!golden_offset) return MB_E_ARGUMENT;
    return set_choice(f,product,1,golden_offset);
}
int mb_clear_request(const mb_flash_t *f)
{
    mb_metadata_t m; int rc;
    if (!flash_valid(f,1)) return MB_E_ARGUMENT;
    rc=mb_metadata_load(f,&m); if (rc!=MB_OK) return rc;
    if (!m.request_product) return MB_OK;
    m.request_product=MB_PRODUCT_NONE;
    return metadata_commit(f,&m);
}
int mb_update_begin(const mb_flash_t *f, uint32_t product)
{
    mb_metadata_t m; mb_image_t legacy; mb_slot_record_t *target; int rc;
    if (!flash_valid(f,1) || !product_valid(product)) return MB_E_ARGUMENT;
    rc=mb_metadata_load(f,&m);
    if (rc==MB_E_READ) return rc;
    if (rc!=MB_OK) {
        rc=metadata_adopt(f,&m,NULL); if (rc!=MB_OK) return rc;
        rc=old_legacy_verify(f,&legacy);
        if (rc==MB_E_READ) return rc;
        if (rc==MB_OK) {
            if (product==MB_PRODUCT_MULTITINI && legacy.image_size>MB_APPLETINI_CAPACITY)
                return MB_E_OVERLAP;
            if (legacy.image_size<=MB_APPLETINI_CAPACITY) record_image(&m.slots[0],&legacy,1);
        }
    }
    target=&m.slots[product_index(product)];
    target->product=product; target->flags=0;
    target->payload_size=target->payload_crc32=target->image_size=target->image_crc32=0;
    return metadata_commit(f,&m);
}
int mb_update_finish(const mb_flash_t *f, const mb_image_t *source)
{
    mb_metadata_t m; mb_image_t installed; mb_slot_record_t record; int rc;
    int legacy=source && source->version==1U;
    if (!flash_valid(f,1) || !source || !product_valid(source->product) ||
        (source->version!=2U && !(legacy && source->product==MB_PRODUCT_APPLETINI)))
        return MB_E_ARGUMENT;
    rc=flash_image(f,mb_product_offset(source->product),mb_product_capacity(source->product),
                  legacy ? MB_PRODUCT_NONE : source->product,legacy,2U,0,&installed);
    if (rc!=MB_OK) return rc;
    if (legacy) {
        rc=identify_legacy_appletini(f,MB_APPLETINI_OFFSET,&installed);
        if (rc!=MB_OK) return rc;
    }
    memset(&record,0,sizeof record); record_image(&record,source,0);
    if (!image_matches(&record,&installed)) return MB_E_VERIFY;
    rc=mb_metadata_load(f,&m); if (rc!=MB_OK) return rc;
    record_image(&m.slots[product_index(source->product)],&installed,legacy);
    /* With no pre-existing verified tagged selection, the first installed
     * tagged image becomes the default. Adoption and explicit owner choices
     * otherwise survive peer installation. Missing defaults still fall back. */
    if (!product_valid(m.default_product)) m.default_product=source->product;
    /* Restoration must run the selected old F immediately, even if M is the
     * saved power-on choice. Preserve that choice independently of this request. */
    if (legacy) m.request_product=MB_PRODUCT_APPLETINI;
    return metadata_commit(f,&m);
}
int mb_install(const mb_flash_t *f, const mb_reader_t *source, mb_image_t *out)
{
    mb_image_t image; uint8_t buf[IO_SIZE],verify[IO_SIZE],ident[4],zero[4]={0};
    uint32_t base,capacity,off; int rc;
    if (!flash_valid(f,1) || !source || !out) return MB_E_ARGUMENT;
    memset(out,0,sizeof *out);
    rc=mb_firmware_validate(source,&image); if (rc!=MB_OK) return rc;
    capacity=mb_product_capacity(image.product); base=mb_product_offset(image.product);
    if (image.image_size>capacity) return MB_E_IMAGE;
    rc=mb_update_begin(f,image.product); if (rc!=MB_OK) return rc;
    /* Invalidate the old header even when power fails during the following
     * erase. Metadata is already clear; withholding XLNX also protects scans. */
    if (f->program(f->context,base+BOOT_IDENT_OFFSET,zero,4)) return MB_E_PROGRAM;
    if (f->erase(f->context,base,capacity)) return MB_E_ERASE;
    if (read_at(source,BOOT_IDENT_OFFSET,ident,4)!=MB_OK || le32(ident)!=BOOT_IDENT) return MB_E_READ;
    for (off=0;off<image.image_size;) {
        uint32_t n=image.image_size-off;
        if (n>IO_SIZE) n=IO_SIZE;
        if (read_at(source,off,buf,n)!=MB_OK) return MB_E_READ;
        if (off<=BOOT_IDENT_OFFSET && BOOT_IDENT_OFFSET-off<n) memset(buf+BOOT_IDENT_OFFSET-off,0xff,4);
        if (f->program(f->context,base+off,buf,n)) return MB_E_PROGRAM;
        if (f->read(f->context,base+off,verify,n)) return MB_E_READ;
        if (memcmp(buf,verify,n)) return MB_E_VERIFY;
        off+=n;
    }
    if (f->program(f->context,base+BOOT_IDENT_OFFSET,ident,4)) return MB_E_PROGRAM;
    rc=mb_update_finish(f,&image);
    if (rc!=MB_OK) return rc;
    *out=image;
    return MB_OK;
}
