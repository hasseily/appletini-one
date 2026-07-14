#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sleep.h"
#include "xiltimer.h"

#include "usb_config.h"
#include "usb_osal.h"
#include "usb_util.h"

#include "cherryusb_platform.h"

typedef struct {
    uint32_t count;
    uint32_t max_count;
} cherry_sem_t;

typedef struct {
    uint32_t locked;
} cherry_mutex_t;

typedef struct {
    uintptr_t *items;
    uint32_t max_msgs;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} cherry_mq_t;

typedef struct cherry_timer {
    struct usb_osal_timer timer;
    struct cherry_timer *next;
    uint32_t deadline_ms;
    uint8_t active;
} cherry_timer_t;

typedef struct {
    usb_thread_entry_t entry;
    void *args;
} cherry_thread_t;

static cherry_timer_t *g_timer_list;
static uint8_t g_osal_polling;

uint32_t cherryusb_baremetal_ms(void)
{
    XTime now = 0U;

    XTime_GetTime(&now);
    return (uint32_t)(((uint64_t)now * 1000ULL) / (uint64_t)COUNTS_PER_SECOND);
}

static uint8_t cherry_time_after_eq(uint32_t now, uint32_t deadline)
{
    return ((int32_t)(now - deadline) >= 0) ? 1U : 0U;
}

void cherryusb_baremetal_osal_poll(void)
{
    cherry_timer_t *timer;
    uint32_t now;

    if (g_osal_polling != 0U) {
        return;
    }
    g_osal_polling = 1U;

    now = cherryusb_baremetal_ms();
    timer = g_timer_list;
    while (timer != NULL) {
        cherry_timer_t *next = timer->next;

        if (timer->active != 0U &&
            cherry_time_after_eq(now, timer->deadline_ms) != 0U) {
            if (timer->timer.is_period) {
                uint32_t period = timer->timer.timeout_ms;
                if (period == 0U) {
                    period = 1U;
                }
                timer->deadline_ms = now + period;
            } else {
                timer->active = 0U;
            }

            if (timer->timer.handler != NULL) {
                timer->timer.handler(timer->timer.argument);
            }
        }

        timer = next;
    }

    g_osal_polling = 0U;
}

usb_osal_thread_t usb_osal_thread_create(const char *name,
                                         uint32_t stack_size,
                                         uint32_t prio,
                                         usb_thread_entry_t entry,
                                         void *args)
{
    cherry_thread_t *thread;

    (void)name;
    (void)stack_size;
    (void)prio;

    thread = (cherry_thread_t *)malloc(sizeof(*thread));
    if (thread == NULL) {
        return NULL;
    }
    thread->entry = entry;
    thread->args = args;
    return (usb_osal_thread_t)thread;
}

void usb_osal_thread_delete(usb_osal_thread_t thread)
{
    free(thread);
}

void usb_osal_thread_schedule_other(void)
{
}

usb_osal_sem_t usb_osal_sem_create(uint32_t initial_count)
{
    cherry_sem_t *sem = (cherry_sem_t *)malloc(sizeof(*sem));

    if (sem == NULL) {
        return NULL;
    }
    sem->count = initial_count;
    sem->max_count = 0xFFFFFFFFU;
    return (usb_osal_sem_t)sem;
}

usb_osal_sem_t usb_osal_sem_create_counting(uint32_t max_count)
{
    cherry_sem_t *sem = (cherry_sem_t *)malloc(sizeof(*sem));

    if (sem == NULL) {
        return NULL;
    }
    sem->count = 0U;
    sem->max_count = (max_count == 0U) ? 1U : max_count;
    return (usb_osal_sem_t)sem;
}

void usb_osal_sem_delete(usb_osal_sem_t sem)
{
    free(sem);
}

int usb_osal_sem_take(usb_osal_sem_t sem, uint32_t timeout)
{
    cherry_sem_t *s = (cherry_sem_t *)sem;
    uint32_t start;

    if (s == NULL) {
        return -1;
    }

    if (timeout == 0U) {
        if (s->count == 0U) {
            return -1;
        }
        s->count--;
        return 0;
    }

    start = cherryusb_baremetal_ms();
    while (1) {
        if (s->count != 0U) {
            s->count--;
            return 0;
        }

        cherryusb_baremetal_osal_poll();
        cherryusb_baremetal_poll_irq();

        if (timeout != USB_OSAL_WAITING_FOREVER &&
            (cherryusb_baremetal_ms() - start) >= timeout) {
            return -1;
        }

        usleep(100U);
    }
}

int usb_osal_sem_give(usb_osal_sem_t sem)
{
    cherry_sem_t *s = (cherry_sem_t *)sem;

    if (s == NULL) {
        return -1;
    }
    if (s->count < s->max_count) {
        s->count++;
    }
    return 0;
}

void usb_osal_sem_reset(usb_osal_sem_t sem)
{
    cherry_sem_t *s = (cherry_sem_t *)sem;

    if (s != NULL) {
        s->count = 0U;
    }
}

usb_osal_mutex_t usb_osal_mutex_create(void)
{
    cherry_mutex_t *mutex = (cherry_mutex_t *)malloc(sizeof(*mutex));

    if (mutex == NULL) {
        return NULL;
    }
    mutex->locked = 0U;
    return (usb_osal_mutex_t)mutex;
}

void usb_osal_mutex_delete(usb_osal_mutex_t mutex)
{
    free(mutex);
}

int usb_osal_mutex_take(usb_osal_mutex_t mutex)
{
    cherry_mutex_t *m = (cherry_mutex_t *)mutex;

    if (m == NULL) {
        return -1;
    }
    m->locked = 1U;
    return 0;
}

int usb_osal_mutex_give(usb_osal_mutex_t mutex)
{
    cherry_mutex_t *m = (cherry_mutex_t *)mutex;

    if (m == NULL) {
        return -1;
    }
    m->locked = 0U;
    return 0;
}

usb_osal_mq_t usb_osal_mq_create(uint32_t max_msgs)
{
    cherry_mq_t *mq;

    if (max_msgs == 0U) {
        return NULL;
    }

    mq = (cherry_mq_t *)malloc(sizeof(*mq));
    if (mq == NULL) {
        return NULL;
    }
    mq->items = (uintptr_t *)calloc(max_msgs, sizeof(uintptr_t));
    if (mq->items == NULL) {
        free(mq);
        return NULL;
    }
    mq->max_msgs = max_msgs;
    mq->head = 0U;
    mq->tail = 0U;
    mq->count = 0U;
    return (usb_osal_mq_t)mq;
}

void usb_osal_mq_delete(usb_osal_mq_t mq)
{
    cherry_mq_t *q = (cherry_mq_t *)mq;

    if (q != NULL) {
        free(q->items);
        free(q);
    }
}

int usb_osal_mq_send(usb_osal_mq_t mq, uintptr_t addr)
{
    cherry_mq_t *q = (cherry_mq_t *)mq;

    if (q == NULL || q->items == NULL || q->count >= q->max_msgs) {
        return -1;
    }

    q->items[q->tail] = addr;
    q->tail = (q->tail + 1U) % q->max_msgs;
    q->count++;
    return 0;
}

int usb_osal_mq_recv(usb_osal_mq_t mq, uintptr_t *addr, uint32_t timeout)
{
    cherry_mq_t *q = (cherry_mq_t *)mq;
    uint32_t start;

    if (q == NULL || q->items == NULL || addr == NULL) {
        return -1;
    }

    if (timeout == 0U) {
        if (q->count == 0U) {
            return -1;
        }
        *addr = q->items[q->head];
        q->head = (q->head + 1U) % q->max_msgs;
        q->count--;
        return 0;
    }

    start = cherryusb_baremetal_ms();
    while (1) {
        if (q->count != 0U) {
            *addr = q->items[q->head];
            q->head = (q->head + 1U) % q->max_msgs;
            q->count--;
            return 0;
        }

        cherryusb_baremetal_osal_poll();
        cherryusb_baremetal_poll_irq();

        if (timeout != USB_OSAL_WAITING_FOREVER &&
            (cherryusb_baremetal_ms() - start) >= timeout) {
            return -1;
        }

        usleep(100U);
    }
}

struct usb_osal_timer *usb_osal_timer_create(const char *name,
                                             uint32_t timeout_ms,
                                             usb_timer_handler_t handler,
                                             void *argument,
                                             bool is_period)
{
    cherry_timer_t *timer = (cherry_timer_t *)malloc(sizeof(*timer));

    (void)name;

    if (timer == NULL) {
        return NULL;
    }

    memset(timer, 0, sizeof(*timer));
    timer->timer.handler = handler;
    timer->timer.argument = argument;
    timer->timer.is_period = is_period;
    timer->timer.timeout_ms = timeout_ms;
    timer->timer.timer = timer;
    timer->next = g_timer_list;
    g_timer_list = timer;
    return &timer->timer;
}

void usb_osal_timer_delete(struct usb_osal_timer *timer)
{
    cherry_timer_t *target = (cherry_timer_t *)timer;
    cherry_timer_t **link = &g_timer_list;

    if (target == NULL) {
        return;
    }

    while (*link != NULL) {
        if (*link == target) {
            *link = target->next;
            free(target);
            return;
        }
        link = &(*link)->next;
    }
}

void usb_osal_timer_start(struct usb_osal_timer *timer)
{
    cherry_timer_t *t = (cherry_timer_t *)timer;
    uint32_t timeout_ms;

    if (t == NULL) {
        return;
    }

    timeout_ms = t->timer.timeout_ms;
    if (timeout_ms == 0U) {
        timeout_ms = 1U;
    }
    t->deadline_ms = cherryusb_baremetal_ms() + timeout_ms;
    t->active = 1U;
}

void usb_osal_timer_stop(struct usb_osal_timer *timer)
{
    cherry_timer_t *t = (cherry_timer_t *)timer;

    if (t != NULL) {
        t->active = 0U;
    }
}

size_t usb_osal_enter_critical_section(void)
{
    return 0U;
}

void usb_osal_leave_critical_section(size_t flag)
{
    (void)flag;
}

void usb_osal_msleep(uint32_t delay)
{
    uint32_t start = cherryusb_baremetal_ms();

    while ((cherryusb_baremetal_ms() - start) < delay) {
        cherryusb_baremetal_osal_poll();
        cherryusb_baremetal_poll_irq();
        usleep(1000U);
    }
}

void *usb_osal_malloc(size_t size)
{
    const uintptr_t align = CONFIG_USB_ALIGN_SIZE;
    const size_t padded = USB_ALIGN_UP(size, CONFIG_USB_ALIGN_SIZE);
    uintptr_t raw;
    uintptr_t aligned;

    raw = (uintptr_t)malloc(padded + align - 1U + sizeof(void *));
    if (raw == (uintptr_t)NULL) {
        return NULL;
    }

    aligned = (raw + sizeof(void *) + align - 1U) & ~(align - 1U);
    ((void **)aligned)[-1] = (void *)raw;
    return (void *)aligned;
}

void usb_osal_free(void *ptr)
{
    if (ptr != NULL) {
        free(((void **)ptr)[-1]);
    }
}
