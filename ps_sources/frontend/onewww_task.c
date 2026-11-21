/* One cooperative task with its own stack. See onewww_task.h. */
#include "onewww_task.h"

#include <stdint.h>
#include <string.h>

#define TASK_STACK_BYTES (128U * 1024U)
/* Saved by a switch: d8-d15 (64 bytes), then r4-r11 and lr (36 bytes). */
#define FRAME_BYTES 100U

static uint8_t s_stack[TASK_STACK_BYTES] __attribute__((aligned(8)));
static uint32_t s_main_sp, s_task_sp;
static onewww_task_fn s_function;
static volatile uint8_t s_alive;

/* Saves the callee-saved registers on the current stack, stores the stack
 * pointer in *save, and continues on the stack `load` (AAPCS, VFP). */
__attribute__((naked, noinline))
static void task_switch(uint32_t *save __attribute__((unused)),
                        uint32_t load __attribute__((unused)))
{
    __asm__ volatile (
        "push  {r4-r11, lr}\n"
        "vpush {d8-d15}\n"
        "mov   r2, sp\n"
        "str   r2, [r0]\n"
        "mov   sp, r1\n"
        "vpop  {d8-d15}\n"
        "pop   {r4-r11, pc}\n");
}

static void task_entry(void)
{
    s_function();
    s_alive = 0U;
    task_switch(&s_task_sp, s_main_sp);     /* does not return */
    for (;;) {}
}

void onewww_task_start(onewww_task_fn function)
{
    /* The first switch pops this frame and "returns" to task_entry with
     * an 8-byte aligned stack. */
    uint32_t *frame = (uint32_t *)(void *)(s_stack + TASK_STACK_BYTES - FRAME_BYTES);

    memset(frame, 0, FRAME_BYTES);
    frame[24] = (uint32_t)(uintptr_t)task_entry;    /* lr, after 16 + 8 words */
    s_task_sp = (uint32_t)(uintptr_t)frame;
    s_function = function;
    s_alive = 1U;
}

int onewww_task_resume(void)
{
    if (!s_alive) return 0;
    task_switch(&s_main_sp, s_task_sp);
    return s_alive;
}

void onewww_task_yield(void)
{
    task_switch(&s_task_sp, s_main_sp);
}

int onewww_task_active(void) { return s_alive; }
