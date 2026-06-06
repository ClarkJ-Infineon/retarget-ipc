/******************************************************************************
 * @file retarget_ipc_host.c
 * @brief HOST side of retarget-ipc — captures printf and routes to shared memory.
 *
 * @details
 * Provides a strong `_write()` symbol that overrides Newlib's weak default.
 * All printf/stdout output is placed into a SPSC ring buffer in shared memory
 * for the DEVICE core to consume.
 *
 * This file is compiled on whichever core is the printf "sender."
 * Activate via: `COMPONENTS += RETARGET_IPC_HOST` in the Makefile.
 *
 * @par Thread Safety:
 * _write() uses FreeRTOS critical sections to serialize concurrent callers.
 * This handles both multi-task printf and (with caveats) ISR-level calls.
 * However, calling printf() from ISR is strongly discouraged (Newlib is
 * not reentrant in ISR context).
 *
 * @warning Do NOT include both retarget-io and retarget-ipc HOST in the
 *          same project — both provide `_write()` and will cause a linker error.
 *
 * @copyright Copyright 2025-2026 Clark Jarvis / Infineon Technologies AG
 * @license SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

#include "retarget_ipc.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ARM memory barrier intrinsics */
#ifndef __DMB
#define __DMB() __asm volatile ("dmb sy" ::: "memory")
#endif

/*******************************************************************************
 * Module State
 ******************************************************************************/

/** Pointer to the shared memory structure (cast from configured address) */
static retarget_ipc_shared_t *const s_shared =
    (retarget_ipc_shared_t *)RETARGET_IPC_SHARED_ADDR;

#ifdef RETARGET_IPC_UART_TEE
static mtb_hal_uart_t *s_uart = NULL;
#endif

/*******************************************************************************
 * Public API — HOST
 ******************************************************************************/

/**
 * @brief Initialize shared memory ring buffer and signal readiness.
 * @see retarget_ipc.h for full documentation.
 */
void retarget_ipc_host_init(void)
{
    /* Zero the entire buffer structure */
    s_shared->head = 0;
    s_shared->tail = 0;
    s_shared->dropped = 0;
    memset((void *)s_shared->data, 0, RETARGET_IPC_BUFFER_SIZE);

    /* Write magic LAST — signals to DEVICE that buffer is ready.
     * DMB ensures all prior writes are visible before magic. */
    __DMB();
    s_shared->magic = RETARGET_IPC_MAGIC;
    __DMB();
}

#ifdef RETARGET_IPC_UART_TEE
/**
 * @brief Register UART for dual output (shared memory + serial).
 * @see retarget_ipc.h for full documentation.
 */
void retarget_ipc_host_set_uart(mtb_hal_uart_t *uart)
{
    s_uart = uart;
}
#endif

/*******************************************************************************
 * _write() Override — Newlib/GCC
 *
 * Strong symbol that captures all printf/stdout/stderr output.
 * Routes to shared memory ring buffer (and optionally UART tee).
 *
 * Uses FreeRTOS critical section to serialize concurrent task access
 * to the ring buffer head pointer.
 ******************************************************************************/

int _write(int fd, const char *ptr, int len)
{
    (void)fd;

    if (ptr == NULL || len <= 0)
    {
        return 0;
    }

    /* Discard silently if shared memory not yet initialized */
    if (s_shared->magic != RETARGET_IPC_MAGIC)
    {
        return len;
    }

    /* Critical section protects ring buffer head from concurrent tasks.
     * Using FROM_ISR variant for maximum safety (works in both task and ISR). */
    UBaseType_t saved_interrupt_status = taskENTER_CRITICAL_FROM_ISR();

    uint32_t head = s_shared->head;
    const uint32_t tail = s_shared->tail;  /* Read once (DEVICE may advance) */
    uint32_t written = 0;

    for (int i = 0; i < len; i++)
    {
        uint32_t next_head = (head + 1U) % RETARGET_IPC_BUFFER_SIZE;
        if (next_head == tail)
        {
            /* Buffer full — count remaining bytes as dropped */
            s_shared->dropped += (uint32_t)(len - i);
            break;
        }
        s_shared->data[head] = (uint8_t)ptr[i];
        head = next_head;
        written++;
    }

    /* Memory barrier before publishing new head (ensures data visible to DEVICE) */
    __DMB();
    s_shared->head = head;

    taskEXIT_CRITICAL_FROM_ISR(saved_interrupt_status);

    /* DMB after exiting critical section ensures head write propagates */
    __DMB();

    /* Optional UART tee — send all bytes regardless of ring buffer state */
#ifdef RETARGET_IPC_UART_TEE
    if (s_uart != NULL)
    {
        size_t remaining = (size_t)len;
        const uint8_t *p = (const uint8_t *)ptr;
        while (remaining > 0)
        {
            size_t uart_len = remaining;
            mtb_hal_uart_write(s_uart, (void *)p, &uart_len);
            p += uart_len;
            remaining -= uart_len;
        }
    }
#endif

    /* Always return len to prevent libc retry loops */
    return len;
}
