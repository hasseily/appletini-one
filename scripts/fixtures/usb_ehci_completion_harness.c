/* Run the real scanner and completion/free path. Only DMA address translation,
 * register storage, cache operations, and OS wakeups are replaced. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_hc_ehci.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); \
        exit(1); \
    } \
} while (0)

static struct ehci_qh_hw test_qh;
static struct ehci_qtd_hw test_qtd;
static struct ehci_hcor test_regs;
static struct ehci_qh_hw *qh_address(uint32_t address)
{
    return (address & ~31U) == 32U ? &test_qh : NULL;
}
static struct ehci_qtd_hw *qtd_address(uint32_t address)
{
    return (address & ~31U) == 64U ? &test_qtd : NULL;
}

/* Hardware links are 32 bits even on the 64-bit host running this test. */
#undef EHCI_ADDR2QH
#undef EHCI_ADDR2QTD
#undef EHCI_HCOR
#define EHCI_ADDR2QH(addr) qh_address((uint32_t)(addr))
#define EHCI_ADDR2QTD(addr) qtd_address((uint32_t)(addr))
#define EHCI_HCOR ((void)bus, &test_regs)
/* The uncalled controller-init code contains native 32-bit DMA casts. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
#include "../../third_party/CherryUSB/port/ehci/usb_hc_ehci.c"
#pragma GCC diagnostic pop

static unsigned completions;
static unsigned errors_logged;
static int completion_result;
static uint32_t logged_token;

size_t usb_osal_enter_critical_section(void) { return 0U; }
void usb_osal_leave_critical_section(size_t flags) { (void)flags; }
int usb_osal_sem_give(usb_osal_sem_t sem) { (void)sem; return 0; }
void usb_dcache_clean(uintptr_t addr, size_t size) { (void)addr; (void)size; }
void usb_dcache_invalidate(uintptr_t addr, size_t size) { (void)addr; (void)size; }
void usb_dcache_flush(uintptr_t addr, size_t size) { (void)addr; (void)size; }

/* Link the complete HCD without hardware startup or the hub worker. */
struct usbh_bus g_usbhost_bus[CONFIG_USBHOST_MAX_BUS];
int cherryusb_printf(const char *format, ...) { (void)format; return 0; }
void usb_osal_msleep(uint32_t delay) { (void)delay; CHECK(0, "unexpected sleep"); }
usb_osal_sem_t usb_osal_sem_create(uint32_t count)
{
    (void)count;
    CHECK(0, "unexpected controller initialization");
    return NULL;
}
void usb_osal_sem_delete(usb_osal_sem_t sem)
{
    (void)sem;
    CHECK(0, "unexpected controller shutdown");
}
int usb_osal_sem_take(usb_osal_sem_t sem, uint32_t timeout)
{
    (void)sem; (void)timeout;
    CHECK(0, "unexpected synchronous wait");
    return -1;
}
uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port)
{
    (void)bus; (void)port;
    CHECK(0, "unexpected port query");
    return USB_SPEED_FULL;
}
void usbh_hub_thread_wakeup(struct usbh_hub *hub)
{
    (void)hub;
    CHECK(0, "unexpected hub event");
}

void cherryusb_host_debug_note_ehci_qtd(uint8_t busid, uint8_t index,
                                      uint32_t token, uint16_t length,
                                      uint16_t remaining)
{
    CHECK(busid == 0U && index == 0U, "wrong descriptor reported");
    CHECK(length == 32U && remaining == 32U, "wrong failed transfer length");
    ++errors_logged;
    logged_token = token;
}

static void complete(void *arg, int result)
{
    CHECK(arg == &test_qh, "completion argument changed");
    CHECK(test_qh.inuse == false && test_qtd.inuse == false,
          "callback ran before descriptors were released");
    ++completions;
    completion_result = result;
}

static void run_case(uint32_t status, uint32_t pid, unsigned cerr,
                     int expected, unsigned wait_active, unsigned toggle)
{
    struct usbh_bus bus = {0};
    struct usbh_urb urb = {0};
    struct usb_endpoint_descriptor ep = {0};
    struct ehci_qh_hw head = {0};
    const uint32_t token = status | pid | QTD_TOKEN_TOGGLE |
        (cerr << QTD_TOKEN_CERR_SHIFT) |
        ((expected < 0 || wait_active) ? (32U << QTD_TOKEN_NBYTES_SHIFT) : 0U);

    memset(&test_qh, 0, sizeof(test_qh));
    memset(&test_qtd, 0, sizeof(test_qtd));
    completions = errors_logged = 0U;
    completion_result = 123;
    logged_token = 0U;
    ep.bmAttributes = USB_ENDPOINT_TYPE_INTERRUPT;
    ep.bEndpointAddress = (pid == QTD_TOKEN_PID_IN) ? 0x81U : 0x01U;
    urb.ep = &ep;
    urb.errorcode = -USB_ERR_BUSY;
    urb.data_toggle = 1U;
    urb.hcpriv = &test_qh;
    urb.complete = complete;
    urb.arg = &test_qh;
    test_qh.inuse = true;
    test_qh.urb = &urb;
    test_qh.first_qtd = 64U;
    test_qh.hw.hlp = QTD_LIST_END;
    head.hw.hlp = 32U;
    test_qtd.inuse = true;
    test_qtd.urb = &urb;
    test_qtd.length = 32U;
    test_qtd.hw.next_qtd = QTD_LIST_END;
    test_qtd.hw.token = token;

    ehci_check_qh(&bus, &head, &test_qh);
    if (wait_active) {
        CHECK(completions == 0U && errors_logged == 0U,
              "active hardware retry was completed or logged as failed");
        CHECK(urb.errorcode == -USB_ERR_BUSY && urb.hcpriv == &test_qh &&
              test_qh.inuse && test_qtd.inuse && head.hw.hlp == 32U,
              "active hardware retry was unlinked or released");
    } else {
        CHECK(completions == 1U && completion_result == expected,
              "wrong completion classification");
        CHECK(urb.hcpriv == NULL && head.hw.hlp == QTD_LIST_END,
              "completed transfer remained linked");
        CHECK(urb.data_toggle == toggle, "wrong next endpoint data toggle");
        CHECK(errors_logged == (expected < 0 ? 1U : 0U),
              "wrong error diagnostic count");
        CHECK(expected >= 0 || logged_token == token,
              "diagnostic lost original hardware token");
    }
}

int main(void)
{
    const uint32_t halt = QTD_TOKEN_STATUS_HALTED;
    const uint32_t xact = QTD_TOKEN_STATUS_XACTERR;
    const uint32_t dbe = QTD_TOKEN_STATUS_DBERR;
    const uint32_t mmf = QTD_TOKEN_STATUS_MMF;
    const uint32_t active = QTD_TOKEN_STATUS_ACTIVE;
    const uint32_t babble = QTD_TOKEN_STATUS_BABBLE;

    for (unsigned direction = 0U; direction < 2U; ++direction) {
        const uint32_t pid = direction ? QTD_TOKEN_PID_IN : QTD_TOKEN_PID_OUT;
        run_case(active | xact, pid, 2U, 0, 1U, 1U);
        run_case(active | dbe, pid, 3U, 0, 1U, 1U);
        run_case(0U, pid, 3U, 32, 0U, 1U);
        run_case(xact | dbe, pid, 3U, 32, 0U, 1U);
        run_case(halt | xact, pid, 0U, -USB_ERR_IO, 0U, 1U);
        run_case(halt | dbe, pid, 0U, -USB_ERR_IO, 0U, 1U);
        run_case(halt, pid, 0U, -USB_ERR_IO, 0U, 1U);
        run_case(halt, pid, 3U, -USB_ERR_STALL, 0U, 0U);
        run_case(halt | xact, pid, 2U, -USB_ERR_STALL, 0U, 0U);
        run_case(halt | babble, pid, 3U, -USB_ERR_BABBLE, 0U, 0U);
    }
    run_case(halt | mmf, QTD_TOKEN_PID_IN, 3U, -USB_ERR_IO, 0U, 1U);
    run_case(active | mmf, QTD_TOKEN_PID_OUT, 3U, 0, 1U, 1U);
    run_case(mmf, QTD_TOKEN_PID_OUT, 3U, 32, 0U, 1U);
    puts("PASS EHCI completion: 23 IN/OUT retry, split, STALL, babble, and success cases");
    return 0;
}
