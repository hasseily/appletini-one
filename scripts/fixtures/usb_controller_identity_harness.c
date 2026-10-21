#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "usb_controller_identity.h"

static usb_controller_device_t controller(uint32_t serial, uint32_t path)
{
    usb_controller_device_t device = {0};
    device.identity.vendor_id = 0x045eU;
    device.identity.product_id = 0x028eU;
    device.identity.descriptor_hash = UINT32_C(0x80000001);
    device.identity.serial_hash = serial;
    device.identity.port_path = path;
    device.identity.transport = USB_CONTROLLER_TRANSPORT_XBOX360;
    device.connected = 1U;
    device.axis_mask = 0x3fU;
    device.buttons_present = 0x7ffU;
    strcpy(device.name, "Xbox 360 controller");
    return device;
}

static void test_reordering_and_disconnect(void)
{
    usb_controller_device_t pad = controller(0x12345678U, 0x11111111U);
    usb_controller_device_t other = controller(0x87654321U, 0x22222222U);
    usb_controller_identity_t saved;
    usb_controller_identity_init();
    usb_controller_identity_set(1U, &pad);
    usb_controller_identity_set(4U, &other);
    assert(usb_controller_identity_bind(2U, &saved));
    assert(usb_controller_identity_resolve(&saved, 2U, 0x12U) == 1U);

    /* Changed hub order and port retain the serial-bearing device's binding. */
    usb_controller_identity_disconnect(1U);
    usb_controller_identity_disconnect(4U);
    pad.identity.port_path = 0x33333333U;
    usb_controller_identity_set(4U, &pad);
    usb_controller_identity_set(1U, &other);
    assert(usb_controller_identity_resolve(&saved, 2U, 0x12U) == 4U);
    assert(usb_controller_identity_resolve(&saved, 2U, 0x02U) == USB_CONTROLLER_SLOT_COUNT);
    usb_controller_identity_disconnect(4U);
    assert(usb_controller_identity_resolve(&saved, 2U, 0x12U) == USB_CONTROLLER_SLOT_COUNT);
    usb_controller_device_t snapshot;
    memset(&snapshot, 0xff, sizeof(snapshot));
    assert(!usb_controller_identity_get(4U, &snapshot));
    assert(snapshot.connected == 0U && snapshot.name[0] == '\0');
    puts("PASS identity survives slot reorder and port change; absent device never falls back");
}

static void test_identical_devices(void)
{
    usb_controller_device_t first = controller(0U, 11U);
    usb_controller_device_t second = controller(0U, 12U);
    usb_controller_identity_t saved;
    usb_controller_identity_init();
    usb_controller_identity_set(0U, &first);
    usb_controller_identity_set(7U, &second);
    assert(usb_controller_identity_bind(1U, &saved));
    usb_controller_identity_set(0U, &second);
    usb_controller_identity_set(7U, &first);
    assert(usb_controller_identity_resolve(&saved, 1U, 0x81U) == 7U);
    first.identity.port_path = 13U;
    usb_controller_identity_set(7U, &first);
    assert(usb_controller_identity_resolve(&saved, 1U, 0x81U) == USB_CONTROLLER_SLOT_COUNT);

    /* Identical models must not match changed interfaces or descriptors. */
    first.identity = saved;
    first.identity.interface_id = 1U;
    usb_controller_identity_set(7U, &first);
    assert(usb_controller_identity_resolve(&saved, 1U, 0x81U) == USB_CONTROLLER_SLOT_COUNT);
    first.identity = saved;
    first.identity.descriptor_hash ^= 1U;
    usb_controller_identity_set(7U, &first);
    assert(usb_controller_identity_resolve(&saved, 1U, 0x81U) == USB_CONTROLLER_SLOT_COUNT);
    first.identity = saved;
    first.identity.transport = USB_CONTROLLER_TRANSPORT_HID;
    usb_controller_identity_set(7U, &first);
    assert(usb_controller_identity_resolve(&saved, 1U, 0x81U) == USB_CONTROLLER_SLOT_COUNT);

    /* A clone returning the same serial twice cannot claim either slot. */
    first = controller(15U, 10U);
    second = controller(15U, 11U);
    usb_controller_identity_set(0U, &first);
    usb_controller_identity_set(7U, &second);
    saved = first.identity;
    assert(usb_controller_identity_resolve(&saved, 1U, 0x81U) == USB_CONTROLLER_SLOT_COUNT);
    assert(usb_controller_identity_resolve(&saved, 1U, 1U) == 0U);
    puts("PASS no-serial devices use hub paths; duplicates and changed descriptors cannot steal bindings");
}

static void test_legacy_and_generic(void)
{
    usb_controller_identity_t saved = {0};
    usb_controller_device_t generic = controller(1U, 1U);
    usb_controller_device_t pad = controller(2U, 2U);
    generic.generic_axes = 1U;
    usb_controller_identity_init();
    assert(usb_controller_identity_resolve(&saved, 3U, 4U) == 2U);
    assert(usb_controller_identity_resolve(&saved, 3U, 2U) == USB_CONTROLLER_SLOT_COUNT);
    assert(usb_controller_identity_resolve(&saved, 0U, 0x82U) == 1U);
    assert(usb_controller_identity_resolve(&saved, 9U, 0xffU) == USB_CONTROLLER_SLOT_COUNT);
    usb_controller_identity_set(1U, &generic);
    usb_controller_identity_set(7U, &pad);
    assert(usb_controller_identity_resolve(&saved, 0U, 0x82U) == 7U);
    assert(usb_controller_identity_resolve(&saved, 2U, 0x82U) == 1U);
    assert(usb_controller_identity_bind(2U, &saved));
    assert(usb_controller_identity_resolve(&saved, 2U, 0x82U) == 1U);
    assert(!usb_controller_identity_bind(0U, &saved));
    assert(!usb_controller_identity_valid(&saved));
    usb_controller_identity_set(7U, NULL);
    assert(!usb_controller_identity_get(7U, NULL));
    memset(generic.name, 'x', sizeof(generic.name));
    usb_controller_identity_set(1U, &generic);
    assert(usb_controller_identity_get(1U, &pad));
    assert(pad.name[sizeof(pad.name) - 1U] == '\0');
    puts("PASS old slot files retain mappings; generic axes require explicit selection; metadata is bounded");
}

static void test_persistence_text(void)
{
    usb_controller_device_t pad = controller(0x12345678U, 0x90abcdefU);
    usb_controller_identity_t restored;
    char text[USB_CONTROLLER_IDENTITY_TEXT_BYTES];
    assert(usb_controller_identity_format(&pad.identity, text, sizeof(text)));
    assert(strcmp(text, "045E:028E:00:01:80000001:12345678:90ABCDEF") == 0);
    assert(usb_controller_identity_parse(text, &restored));
    assert(usb_controller_identity_equal(&pad.identity, &restored));
    assert(usb_controller_identity_parse("045e:028e:00:01:80000001:12345678:90abcdef", &restored));
    assert(usb_controller_identity_equal(&pad.identity, &restored));
    const char *invalid[] = {NULL, "", "NoNe", " ", "None ",
        "045E:028E:00:01:80000001:12345678:90ABCDE", /* short */
        "045E:028E:00:01:80000001:12345678:90ABCDEFX", /* trailing */
        "045E:028E:00:01:80000001:12345678:90ABCDEG", /* nonhex */
        "045E:028E:00:FF:80000001:12345678:90ABCDEF", /* unknown transport */
        "0000:028E:00:01:80000001:12345678:90ABCDEF", /* no vendor */
        "045E:028E:00:01:80000001:00000000:00000000", /* no instance */
        "045E-028E:00:01:80000001:12345678:90ABCDEF", /* separator */
        "045E:028E:0:01:80000001:12345678:90ABCDEF"};
    for (unsigned i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        restored = pad.identity;
        assert(!usb_controller_identity_parse(invalid[i], &restored));
        assert(!usb_controller_identity_valid(&restored));
    }
    for (size_t size = 1U; size < strlen(text) + 1U; ++size) {
        char small[USB_CONTROLLER_IDENTITY_TEXT_BYTES];
        assert(!usb_controller_identity_format(&pad.identity, small, size));
        assert(small[0] == '\0');
    }
    assert(usb_controller_identity_parse("None", &restored));
    assert(usb_controller_identity_format(&restored, text, sizeof(text)));
    assert(strcmp(text, "None") == 0);
    assert(usb_controller_identity_hash(UINT32_C(2166136261), "hello", 5U) == UINT32_C(0x4f9f2cab));
    puts("PASS persistence round trip, invalid fields, output bounds and stable descriptor hash");
}

int main(void)
{
    test_reordering_and_disconnect();
    test_identical_devices();
    test_legacy_and_generic();
    test_persistence_text();
    return 0;
}
