/******************************************************************************
 * File: retarget_ipc_host.c
 *
 * Description: HOST side of retarget-ipc library.
 *              Provides _write() override that routes printf output to a
 *              shared memory ring buffer for consumption by the DEVICE core.
 *
 *              Compiled on the application/host core (e.g., CM33_NS).
 *              Requires: COMPONENT_RETARGET_IPC_HOST in Makefile.
 *
 * Copyright 2026 Infineon Technologies AG
 * SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

#include "retarget_ipc.h"
#include <string.h>

/* ARM memory barrier intrinsics */
#ifndef __DMB
#define __DMB() __asm volatile ("dmb sy" ::: "memory")
#endif

/*******************************************************************************
 * Module State
 ******************************************************************************/

/** Pointer to the shared memory structure */
static retarget_ipc_shared_t *const s_shared =
    (retarget_ipc_shared_t *)RETARGET_IPC_SHARED_ADDR;

#ifdef RETARGET_IPC_UART_TEE
static mtb_hal_uart_t *s_uart = NULL;
#endif

/*******************************************************************************
 * Public API — HOST
 ******************************************************************************/

void retarget_ipc_host_init(void)
{
    /* Initialize the shared memory ring buffer */
    s_shared->head = 0;
    s_shared->tail = 0;
    s_shared->dropped = 0;
    memset((void *)s_shared->data, 0, RETARGET_IPC_BUFFER_SIZE);

    /* Write magic last — signals to device that buffer is ready */
    __DMB();
    s_shared->magic = RETARGET_IPC_MAGIC;
    __DMB();
}

#ifdef RETARGET_IPC_UART_TEE
void retarget_ipc_host_set_uart(mtb_hal_uart_t *uart)
{
    s_uart = uart;
}
#endif

/*******************************************************************************
 * _write() Override — Newlib/GCC
 *
 * This is a STRONG symbol that captures all printf/stdout output and routes
 * it to the shared memory ring buffer (and optionally UART tee).
 *
 * NOTE: This replaces retarget-io's weak _write(). Do NOT include both
 * retarget-io and retarget-ipc HOST in the same project.
 ******************************************************************************/

int _write(int fd, const char *ptr, int len)
{
    (void)fd;

    if (ptr == NULL || len <= 0)
    {
        return 0;
    }

    /* Verify shared memory is initialized */
    if (s_shared->magic != RETARGET_IPC_MAGIC)
    {
        return len;  /* Silently discard if not initialized */
    }

    /* Write to shared ring buffer (SPSC: we are the only writer) */
    uint32_t head = s_shared->head;
    const uint32_t tail = s_shared->tail;  /* Read once (device may update) */
    uint32_t written = 0;

    for (int i = 0; i < len; i++)
    {
        uint32_t next_head = (head + 1U) % RETARGET_IPC_BUFFER_SIZE;
        if (next_head == tail)
        {
            /* Buffer full — count dropped bytes */
            s_shared->dropped += (uint32_t)(len - i);
            break;
        }
        s_shared->data[head] = (uint8_t)ptr[i];
        head = next_head;
        written++;
    }

    /* Memory barrier before publishing new head (ensures data is visible) */
    __DMB();
    s_shared->head = head;
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

    /* Always return len to avoid libc retries */
    return len;
}
