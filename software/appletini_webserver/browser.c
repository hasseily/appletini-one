/* Fixed-page HTTP browser demo for an NMOS 6502. */

#include <conio.h>
#include <stdint.h>
#include <string.h>

#include "appletini_net.h"
#include "ip65_min.h"

#define APPLETINI_URL "http://httpbin.io/headers"
#define RESPONSE_CAP  1600U

static char response[RESPONSE_CAP];

static void wait_for_key(void)
{
    cputs("\r\nPRESS ANY KEY\r\n");
    (void)cgetc();
}

static void print_text(const char *text)
{
    char value;

    while ((value = *text++) != '\0') {
        if (value == '\r') {
            if (*text == '\n') {
                ++text;
            }
            cputs("\r\n");
        } else if (value == '\n') {
            cputs("\r\n");
        } else if ((uint8_t)value >= 0x20U && (uint8_t)value < 0x7FU) {
            cputc(value);
        }
    }
}

static void show_response(uint16_t length)
{
    char *body;
    char *status_end;

    response[length] = '\0';
    body = strstr(response, "\r\n\r\n");
    status_end = strstr(response, "\r\n");
    cputs("\r\n");
    if (status_end != NULL) {
        *status_end = '\0';
        print_text(response);
    }
    cputs("\r\n\r\n");
    print_text(body != NULL ? body + 4 : response);
}

int main(void)
{
    uint8_t status;
    uint16_t length;

    clrscr();
    cputs("APPLETINI WEB BROWSER\r\n\r\n");
    cputs(APPLETINI_URL "\r\n\r\n");

    status = appletini_network_init();
    if (status != APPLETINI_NET_OK) {
        cputs(appletini_network_error(status));
        cputs("\r\n");
        wait_for_key();
        return 1;
    }
    if ((appletini_config.gateway[0] | appletini_config.gateway[1] |
         appletini_config.gateway[2] | appletini_config.gateway[3]) == 0U) {
        cputs("CARD HAS NO SAVED GATEWAY\r\n");
        wait_for_key();
        return 1;
    }

    cputs("CONNECTING...\r\n");
    length = url_download(APPLETINI_URL,
                          (const uint8_t *)response,
                          RESPONSE_CAP - 1U);
    if (length == 0U) {
        cputs("REQUEST FAILED: ");
        cputs(ip65_strerror(ip65_error));
        cputs("\r\n");
        wait_for_key();
        return 1;
    }

    show_response(length);
    wait_for_key();
    return 0;
}
