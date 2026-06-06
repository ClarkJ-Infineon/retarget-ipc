/******************************************************************************
 * @file retarget_ipc_device.c
 * @brief DEVICE side of retarget-ipc — reads printf data from shared memory.
 *
 * @details
 * Provides API to read printf data sent by the HOST core via the shared
 * memory SPSC ring buffer. The DEVICE core polls this buffer and forwards
 * the data to its local output backend (retarget-io UART, retarget-lvgl
 * display, or custom handler).
 *
 * This file is compiled on whichever core is the "receiver."
 * Activate via: `COMPONENTS += RETARGET_IPC_DEVICE` in the Makefile.
 *
 * @par Usage Example (relay to UART):
 * @code
 * void relay_task(void *arg) {
 *     uint8_t buf[256];
 *     retarget_ipc_device_init();
 *     while (1) {
 *         uint32_t n = retarget_ipc_read(buf, sizeof(buf));
 *         if (n > 0) {
 *             // Forward to local UART or other backend
 *             hal_uart_write(buf, n);
 *         }
 *         vTaskDelay(pdMS_TO_TICKS(10));
 *     }
 * }
 * @endcode
 *
 * @copyright Copyright 2025-2026 Clark Jarvis / Infineon Technologies AG
 * @license SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

#include "retarget_ipc.h"
#include <stddef.h>

/* ARM memory barrier intrinsics */
#ifndef __DMB
#define __DMB() __asm volatile ("dmb sy" ::: "memory")
#endif

/*******************************************************************************
 * Module State
 ******************************************************************************/

/** Pointer to the shared memory structure (same address as HOST) */
static retarget_ipc_shared_t *const s_shared =
    (retarget_ipc_shared_t *)RETARGET_IPC_SHARED_ADDR;

/** Flag indicating retarget_ipc_device_init() has been called */
static bool s_initialized = false;

/*******************************************************************************
 * Public API — DEVICE
 ******************************************************************************/

void retarget_ipc_device_init(void)
{
    /* We do NOT write the buffer structure — HOST owns initialization.
     * Just mark ourselves as ready to receive. The magic check in
     * retarget_ipc_read() ensures we don't read garbage. */
    s_initialized = true;
}

bool retarget_ipc_is_active(void)
{
    if (!s_initialized)
    {
        return false;
    }
    __DMB();
    return (s_shared->magic == RETARGET_IPC_MAGIC);
}

uint32_t retarget_ipc_read(uint8_t *buf, uint32_t max_len)
{
    if (buf == NULL || max_len == 0 || !s_initialized)
    {
        return 0;
    }

    /* Verify HOST has initialized the channel */
    if (s_shared->magic != RETARGET_IPC_MAGIC)
    {
        return 0;
    }

    /* DMB ensures we see the latest head value written by HOST */
    __DMB();

    uint32_t tail = s_shared->tail;
    const uint32_t head = s_shared->head;  /* Read once (HOST may advance) */
    uint32_t count = 0;

    while (tail != head && count < max_len)
    {
        buf[count] = s_shared->data[tail];
        tail = (tail + 1U) % RETARGET_IPC_BUFFER_SIZE;
        count++;
    }

    if (count > 0)
    {
        /* DMB before publishing new tail (ensures reads complete before
         * HOST sees freed space and potentially overwrites) */
        __DMB();
        s_shared->tail = tail;
        __DMB();
    }

    return count;
}

uint32_t retarget_ipc_get_dropped(void)
{
    if (!s_initialized || s_shared->magic != RETARGET_IPC_MAGIC)
    {
        return 0;
    }

    __DMB();
    uint32_t dropped = s_shared->dropped;
    if (dropped > 0)
    {
        /* Atomic-ish clear — HOST only increments, we only clear.
         * Small race window acceptable for diagnostics. */
        s_shared->dropped = 0;
        __DMB();
    }
    return dropped;
}
