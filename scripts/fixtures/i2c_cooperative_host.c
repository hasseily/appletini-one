#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "i2c.h"
#include "xiltimer.h"
#include "xstatus.h"

#define ADDRESS 0x48U
#define REGISTER 0x01U
#define INITIAL_CONTROL 0x570EU
#define INITIAL_MASK (XIICPS_IXR_ALL_INTR_MASK & ~XIICPS_IXR_SLV_RDY_MASK)
#define SEND_EVENTS (XIICPS_IXR_COMP_MASK | XIICPS_IXR_NACK_MASK | \
                     XIICPS_IXR_ARB_LOST_MASK | XIICPS_IXR_TO_MASK)
#define RECEIVE_EVENTS (SEND_EVENTS | XIICPS_IXR_DATA_MASK | XIICPS_IXR_RX_OVR_MASK)

typedef enum {
    IDLE,
    SEND,
    SEND_STOP,
    RECEIVE,
    RECEIVE_STOP,
    PHASE_COUNT
} phase_t;

typedef enum {
    SUCCESS,
    SEND_NACK,
    SEND_TIMEOUT,
    RECEIVE_NACK,
    RECEIVE_STUCK_BUSY
} scenario_t;

typedef struct {
    scenario_t scenario;
    phase_t phase;
    u32 control;
    u32 interrupt_status;
    u32 interrupt_mask;
    XTime ticks;
    u8 *receive_data;
    s32 receive_count;
    unsigned checkpoints[PHASE_COUNT];
    unsigned sends;
    unsigned receives;
    unsigned polled_sends;
    unsigned polled_receives;
    unsigned interrupts;
    unsigned aborts;
} mock_t;

static XIicPs controller;
static unsigned original_ref;
static mock_t mock;

static void original_handler(void *ref, u32 events)
{
    (void)ref;
    (void)events;
    assert(!"transfer called the controller's previous status handler");
}

static void reset_mock(scenario_t scenario)
{
    controller = (XIicPs) {
        .Config = {XPAR_XIICPS_0_BASEADDR},
        .IsReady = XIL_COMPONENT_IS_READY,
        .StatusHandler = original_handler,
        .CallBackRef = &original_ref,
    };
    mock = (mock_t) {
        .scenario = scenario,
        .phase = IDLE,
        .control = INITIAL_CONTROL,
        .interrupt_mask = INITIAL_MASK,
    };
}

static void fill_data(u8 *data, s32 len)
{
    for (s32 i = 0; i < len; i++) {
        data[i] = (u8)(0xA0U + (unsigned)i);
    }
}

static void check_data(const u8 *data, s32 len)
{
    for (s32 i = 0; i < len; i++) {
        assert(data[i] == (u8)(0xA0U + (unsigned)i));
    }
}

XIicPs_Config *XIicPs_LookupConfig(UINTPTR base)
{
    assert(base == controller.Config.BaseAddress);
    return &controller.Config;
}

s32 XIicPs_CfgInitialize(XIicPs *i2c, XIicPs_Config *config, UINTPTR base)
{
    assert(i2c == &controller && config == &controller.Config);
    assert(base == controller.Config.BaseAddress);
    return XST_SUCCESS;
}

s32 XIicPs_SetSClk(XIicPs *i2c, u32 frequency)
{
    assert(i2c == &controller && frequency == 100000U);
    return XST_SUCCESS;
}

static void check_send(XIicPs *i2c, const u8 *data, s32 len, u16 address)
{
    assert(i2c == &controller && mock.phase == IDLE);
    assert(address == ADDRESS && len == 1 && data[0] == REGISTER);
}

s32 XIicPs_MasterSendPolled(XIicPs *i2c, u8 *data, s32 len, u16 address)
{
    check_send(i2c, data, len, address);
    assert(mock.polled_sends++ == 0U);
    return XST_SUCCESS;
}

s32 XIicPs_MasterRecvPolled(XIicPs *i2c, u8 *data, s32 len, u16 address)
{
    assert(i2c == &controller && mock.phase == IDLE);
    assert(address == ADDRESS && mock.polled_sends == 1U);
    assert(mock.polled_receives++ == 0U);
    fill_data(data, len);
    return XST_SUCCESS;
}

s32 XIicPs_BusIsBusy(XIicPs *i2c)
{
    assert(i2c == &controller);
    return mock.phase != IDLE;
}

void XIicPs_SetStatusHandler(XIicPs *i2c, void *ref, XIicPs_IntrHandler handler)
{
    assert(i2c == &controller);
    i2c->StatusHandler = handler;
    i2c->CallBackRef = ref;
}

void XIicPs_MasterSend(XIicPs *i2c, u8 *data, s32 len, u16 address)
{
    check_send(i2c, data, len, address);
    assert(mock.sends++ == 0U && i2c->StatusHandler != original_handler);
    mock.phase = SEND;
    /* DATA is pending but masked during a one-byte send. */
    mock.interrupt_status = XIICPS_IXR_DATA_MASK;
    mock.interrupt_mask = XIICPS_IXR_ALL_INTR_MASK & ~SEND_EVENTS;
}

void XIicPs_MasterRecv(XIicPs *i2c, u8 *data, s32 len, u16 address)
{
    assert(i2c == &controller && mock.phase == IDLE);
    assert(address == ADDRESS && mock.sends == 1U && len <= XIICPS_FIFO_DEPTH);
    assert(mock.receives++ == 0U && mock.checkpoints[SEND_STOP] != 0U);
    mock.phase = RECEIVE;
    mock.receive_data = data;
    mock.receive_count = len;
    mock.interrupt_status = 0U;
    mock.interrupt_mask = XIICPS_IXR_ALL_INTR_MASK & ~RECEIVE_EVENTS;
}

void XIicPs_MasterInterruptHandler(XIicPs *i2c)
{
    assert(i2c == &controller);
    const u32 pending = mock.interrupt_status & ~mock.interrupt_mask;
    assert(pending != 0U);
    assert(mock.phase == SEND || mock.phase == RECEIVE);
    mock.interrupt_status = 0U;
    mock.interrupts++;
    u32 events = 0U;
    if ((pending & XIICPS_IXR_COMP_MASK) != 0U) {
        if (mock.phase == SEND) {
            events |= XIICPS_EVENT_COMPLETE_SEND;
        } else {
            events |= XIICPS_EVENT_COMPLETE_RECV;
            fill_data(mock.receive_data, mock.receive_count);
        }
    }
    if ((pending & XIICPS_IXR_NACK_MASK) != 0U) {
        events |= XIICPS_EVENT_ERROR | XIICPS_EVENT_NACK;
    } else {
        mock.phase = mock.phase == SEND ? SEND_STOP : RECEIVE_STOP;
    }
    i2c->StatusHandler(i2c->CallBackRef, events);
}

void XIicPs_Abort(XIicPs *i2c)
{
    assert(i2c == &controller && mock.aborts++ == 0U);
    assert(mock.interrupt_mask == XIICPS_IXR_ALL_INTR_MASK);
    mock.phase = IDLE;
    mock.control = 0U;
    mock.interrupt_status |= XIICPS_IXR_COMP_MASK;
}

u32 XIicPs_ReadReg(UINTPTR base, u32 offset)
{
    assert(base == controller.Config.BaseAddress);
    switch (offset) {
    case XIICPS_CR_OFFSET: return mock.control;
    case XIICPS_SR_OFFSET: return mock.phase == IDLE ? 0U : XIICPS_SR_BA_MASK;
    case XIICPS_ISR_OFFSET: return mock.interrupt_status;
    case XIICPS_IMR_OFFSET: return mock.interrupt_mask;
    default: assert(!"unexpected I2C register read"); return 0U;
    }
}

void XIicPs_WriteReg(UINTPTR base, u32 offset, u32 value)
{
    assert(base == controller.Config.BaseAddress);
    switch (offset) {
    case XIICPS_CR_OFFSET: mock.control = value; break;
    case XIICPS_ISR_OFFSET: mock.interrupt_status &= ~value; break;
    case XIICPS_IER_OFFSET: mock.interrupt_mask &= ~value; break;
    case XIICPS_IDR_OFFSET: mock.interrupt_mask |= value; break;
    default: assert(!"unexpected I2C register write");
    }
}

void XTime_GetTime(XTime *time)
{
    mock.ticks += ((XTime)(COUNTS_PER_SECOND)) / 20000U; /* 50 us per sample */
    *time = mock.ticks;
}

static void checkpoint(void)
{
    const unsigned polls = ++mock.checkpoints[mock.phase];
    if (mock.phase == SEND && polls == 3U && mock.scenario != SEND_TIMEOUT) {
        mock.interrupt_status |= XIICPS_IXR_COMP_MASK;
        if (mock.scenario == SEND_NACK) {
            mock.interrupt_status |= XIICPS_IXR_NACK_MASK;
        }
    } else if (mock.phase == RECEIVE && polls == 3U) {
        mock.interrupt_status |= mock.scenario == RECEIVE_NACK ?
            XIICPS_IXR_NACK_MASK : XIICPS_IXR_COMP_MASK;
    } else if (mock.phase == SEND_STOP && polls == 2U) {
        mock.phase = IDLE;
    } else if (mock.phase == RECEIVE_STOP && polls == 2U && mock.scenario != RECEIVE_STUCK_BUSY) {
        mock.phase = IDLE;
    }
}

static void check_cleanup(void)
{
    assert(controller.StatusHandler == original_handler && controller.CallBackRef == &original_ref);
    assert(mock.interrupt_mask == INITIAL_MASK && mock.interrupt_status == 0U);
}

static void test_legacy(int length, void (*callback)(void))
{
    u8 data[XIICPS_FIFO_DEPTH + 1];
    reset_mock(SUCCESS);
    i2c_set_wait_checkpoint(callback);
    assert(i2c_read_bytes(&controller, ADDRESS, REGISTER, data, length) == 0);
    check_data(data, length);
    assert(mock.polled_sends == 1U && mock.polled_receives == 1U);
    assert(mock.sends == 0U && mock.receives == 0U && mock.ticks == 0U);
    for (unsigned i = 0; i < PHASE_COUNT; i++) {
        assert(mock.checkpoints[i] == 0U);
    }
    check_cleanup();
}

static void test_busy_entry(void)
{
    u8 data[2];
    reset_mock(SUCCESS);
    mock.phase = SEND_STOP;
    i2c_set_wait_checkpoint(checkpoint);
    assert(i2c_read_bytes(&controller, ADDRESS, REGISTER, data, sizeof(data)) == -2);
    assert(mock.phase == SEND_STOP && mock.aborts == 0U);
    assert(mock.sends == 0U && mock.receives == 0U && mock.ticks == 0U);
    assert(mock.polled_sends == 0U && mock.polled_receives == 0U);
    check_cleanup();
}

static void test_cooperative(scenario_t scenario, int expected, unsigned receives,
                             unsigned interrupts)
{
    u8 data[XIICPS_FIFO_DEPTH];
    reset_mock(scenario);
    i2c_set_wait_checkpoint(checkpoint);
    assert(i2c_read_bytes(&controller, ADDRESS, REGISTER, data, sizeof(data)) == expected);
    assert(mock.sends == 1U && mock.receives == receives && mock.interrupts == interrupts);
    assert(mock.polled_sends == 0U && mock.polled_receives == 0U);
    assert(mock.checkpoints[SEND] >= 3U);
    if (expected == 0) {
        check_data(data, sizeof(data));
        assert(mock.aborts == 0U && mock.checkpoints[RECEIVE] >= 3U);
        assert(mock.checkpoints[SEND_STOP] >= 2U && mock.checkpoints[RECEIVE_STOP] >= 2U);
    } else {
        assert(mock.aborts == 1U && mock.control == INITIAL_CONTROL);
    }
    if (scenario == SEND_TIMEOUT || scenario == RECEIVE_STUCK_BUSY) {
        assert(mock.ticks >= (COUNTS_PER_SECOND) / 50U);
    }
    assert(mock.phase == IDLE);
    check_cleanup();
}

int main(void)
{
    test_legacy(2, NULL);
    test_legacy(XIICPS_FIFO_DEPTH + 1, checkpoint);
    test_busy_entry();
    test_cooperative(SUCCESS, 0, 1U, 2U);
    test_cooperative(SEND_NACK, -2, 0U, 1U);
    test_cooperative(SEND_TIMEOUT, -2, 0U, 0U);
    test_cooperative(RECEIVE_NACK, -3, 1U, 2U);
    test_cooperative(RECEIVE_STUCK_BUSY, -4, 1U, 2U);
    test_cooperative(SUCCESS, 0, 1U, 2U);
    i2c_set_wait_checkpoint(NULL);
    puts("I2C cooperative host checks passed");
    return 0;
}
