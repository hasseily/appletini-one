/* Native adapter for the production release parser and read-only image checks. */
#include "online_update_core.h"
#include "multiboot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *bytes;
static size_t length;
static int load(const char *path)
{
    FILE *file=fopen(path,"rb");
    if (!file) return 0;
    if (fseek(file,0,SEEK_END)) { fclose(file); return 0; }
    long n=ftell(file);
    if (n<0 || fseek(file,0,SEEK_SET)) { fclose(file); return 0; }
    bytes=malloc((size_t)n+1U);
    if (!bytes) { fclose(file); return 0; }
    length=(size_t)n;
    int ok=fread(bytes,1,length,file)==length;
    fclose(file); return ok;
}
static int read_image(void *context,uint32_t at,void *out,uint32_t n)
{
    (void)context;
    if (at>length || n>length-at) return -1;
    memcpy(out,bytes+at,n); return 0;
}
int main(int argc,char **argv)
{
    if (argc==5 && !strcmp(argv[1],"compare")) {
        int cmp=99,rc=online_update_version_compare(argv[2],argv[3],argv[4][0],&cmp);
        printf("%d %d\n",rc,cmp); return 0;
    }
    if (argc<3 || !load(argv[2])) return 2;
    if (!strcmp(argv[1],"image")) {
        if (argc!=4 || length>UINT32_MAX) return 2;
        mb_reader_t reader={NULL,read_image,(uint32_t)length};
        mb_image_t image;
        char version[32]={0};
        int rc=mb_update_image_validate(&reader,(uint8_t)atoi(argv[3]),&image,version,sizeof version);
        printf("%d %s\n",rc,version); free(bytes); return 0;
    }
    if (!strcmp(argv[1],"parse")) {
        static online_update_core_t core;
        size_t chunk=argc>3?(size_t)strtoul(argv[3],NULL,10):length;
        size_t budget=argc>4?(size_t)strtoul(argv[4],NULL,10):31;
        if (!chunk || !budget) return 2;
        size_t available=0;
        int rc=0;
        online_update_core_init(&core,bytes);
        for (unsigned turns=0;turns<200000U && !rc;++turns) {
            if (available<length) {
                size_t n=length-available;
                if (n>chunk) n=chunk;
                available+=n;
            }
            rc=online_update_core_feed(&core,available,available==length,budget);
        }
        printf("%d %s %u %u %u %u\n",rc,core.release.tag,
               core.release.assets[0].present,core.release.assets[0].size,
               core.release.assets[1].present,core.release.assets[1].size);
        free(bytes); return 0;
    }
    free(bytes); return 2;
}
