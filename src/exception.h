/* SPDX-License-Identifier: MIT */

#ifndef __EXCEPTION_H__
#define __EXCEPTION_H__

#define SIZEOF_EXC_INFO (64 * 8)

#ifndef __ASSEMBLER__

#include <assert.h>
#include <stdint.h>

#include "types.h"

enum exc_guard_t {
    GUARD_OFF = 0,
    GUARD_SKIP,
    GUARD_MARK,
    GUARD_RETURN,
    GUARD_TYPE_MASK = 0xff,
    GUARD_SILENT = 0x100,
};

struct exc_info {
    u64 regs[32];
    u64 spsr;
    u64 elr;
    u64 esr;
    u64 far;
    u64 afsr1;
    u64 sp[3];
    u64 cpu_id;
    u64 mpidr;
    u64 elr_phys;
    u64 far_phys;
    u64 sp_phys;
    void *extra;
};
static_assert(sizeof(struct exc_info) <= SIZEOF_EXC_INFO, "Please increase SIZEOF_EXC_INFO");
static_assert((sizeof(struct exc_info) & 15) == 0, "SIZEOF_EXC_INFO must be a multiple of 16");

extern volatile enum exc_guard_t exc_guard;
extern volatile int exc_count;

void exception_initialize(void);
void exception_shutdown(void);

void print_regs(u64 *regs, int el12);

uint64_t el0_call(void *func, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
uint64_t el1_call(void *func, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
uint64_t el3_call(void *func, uint64_t a, uint64_t b, uint64_t c, uint64_t d);

struct spmi_irq_event {
    u32 regs [9];
};

struct irq_event {
    u64 time;
    u32 num;
    struct spmi_irq_event spmi;
};

#define IRQ_EVENT_QUEUE_SIZE 64
struct irq_event_queue {
    struct irq_event events [IRQ_EVENT_QUEUE_SIZE];
    volatile size_t read_cursor, write_cursor;
    size_t read_cursor_local, write_cursor_local;
    volatile bool overflow;
};
extern struct irq_event_queue irq_event_queue;
static inline struct irq_event *irq_event_queue_write_alloc(void) {
    if (irq_event_queue.write_cursor_local - irq_event_queue.read_cursor >= IRQ_EVENT_QUEUE_SIZE) {
        irq_event_queue.overflow = true;
        return NULL;
    }
    return &irq_event_queue.events[(irq_event_queue.write_cursor_local++) % IRQ_EVENT_QUEUE_SIZE];
}
static inline void irq_event_queue_write_commit(void) {
    irq_event_queue.write_cursor = irq_event_queue.write_cursor_local;
}
static inline struct irq_event *irq_event_queue_read_alloc(void) {
    if (irq_event_queue.write_cursor - irq_event_queue.read_cursor_local == 0) {
        return NULL;
    }
    return &irq_event_queue.events[(irq_event_queue.read_cursor_local++) % IRQ_EVENT_QUEUE_SIZE];
}
static inline void irq_event_queue_read_commit(void) {
    irq_event_queue.read_cursor = irq_event_queue.read_cursor_local;
}

#endif

#endif
