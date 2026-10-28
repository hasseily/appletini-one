/* Stock-firmware disk write check. Uses only ProDOS file calls. */
#include <conio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned char data[512], result[32];
static unsigned char case_id, stage, target, got, expected;
static unsigned char saved_errno, saved_oserror;
static unsigned int round_index, mismatch, sp_done, d2_done;

static unsigned char pattern(unsigned int at)
{
    return (unsigned char)(at * 17U + round_index * 29U + 0x53U);
}

static unsigned char check(const char *path)
{
    int fd;
    unsigned int at;

    for (at = 0; at < 512; ++at) data[at] = pattern(at);
    stage = 1;
    fd = open(path, O_RDWR);
    if (fd < 0) return 0;
    stage = 2;
    if (write(fd, data, 512) != 512) {
        saved_errno = errno; saved_oserror = _oserror;
        close(fd);
        return 0;
    }
    stage = 3;
    if (close(fd) != 0) return 0;
    stage = 4;
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    stage = 5;
    if (read(fd, data, 512) != 512) {
        saved_errno = errno; saved_oserror = _oserror;
        close(fd);
        return 0;
    }
    stage = 6;
    if (close(fd) != 0) return 0;
    stage = 7;
    for (at = 0; at < 512; ++at) {
        if (data[at] != pattern(at)) {
            mismatch = at; got = data[at]; expected = pattern(at);
            return 0;
        }
    }
    stage = 0;
    return 1;
}

static void put16(unsigned char *p, unsigned int n)
{
    p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8);
}

static unsigned char save_result(unsigned char passed)
{
    int fd;
    char path[] = "/D2HOST/CASE0.BIN";

    path[12] = case_id;
    memset(result, 0, sizeof(result));
    memcpy(result, "D2WC", 4);
    result[4] = 1; result[5] = case_id; result[6] = passed;
    result[7] = target; result[8] = stage;
    result[9] = saved_errno; result[10] = saved_oserror;
    put16(result + 12, round_index); put16(result + 14, sp_done);
    put16(result + 16, d2_done); put16(result + 18, mismatch);
    result[20] = got; result[21] = expected;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return 0;
    if (write(fd, result, sizeof(result)) != sizeof(result)) {
        close(fd);
        return 0;
    }
    return close(fd) == 0;
}

int main(void)
{
    unsigned char passed = 0;

    clrscr();
    cprintf("STOCK DISK II WRITE CHECK\r\nCASE 1..5: ");
    case_id = cgetc();
    if (case_id < '1' || case_id > '5') return 1;
    cprintf("%c\r\nSMARTPORT CONTROL: 8 ROUNDS\r\n", case_id);
    target = 0;
    for (round_index = 0; round_index < 8; ++round_index) {
        if (!check("/D2HOST/SCRATCH")) goto finish;
        ++sp_done;
    }
    cprintf("SMARTPORT PASS\r\nDISK II: 32 ROUNDS\r\n");
    target = 1;
    for (round_index = 0; round_index < 32; ++round_index) {
        if (!check("/D2TEST/SCRATCH")) goto finish;
        ++d2_done;
        if ((d2_done & 7) == 0) cprintf("%u DISK II ROUNDS OK\r\n", d2_done);
    }
    passed = 1;
finish:
    if (stage != 0 && stage != 7 && saved_errno == 0) {
        saved_errno = errno; saved_oserror = _oserror;
    }
    cprintf("%s TARGET=%u STAGE=%u ROUND=%u\r\n",
            passed ? "PASS" : "FAIL", target, stage, round_index);
    cprintf("SP=%u D2=%u E=%u OS=%02X\r\n",
            sp_done, d2_done, saved_errno, saved_oserror);
    if (stage == 7) cprintf("AT %u GOT=%02X EXP=%02X\r\n", mismatch, got, expected);
    cprintf("RESULT SAVE %s\r\n", save_result(passed) ? "OK" : "FAILED");
    cprintf("DONE; PRESS A KEY\r\n");
    cgetc();
    return !passed;
}
