/******************************************************************************
 * File: retarget_ipc_device.c
 *
 * Description: DEVICE side of retarget-ipc library.
 *              Provides API to read printf data sent from the HOST core
 *              via shared memory ring buffer.
 *
 *              Compiled on the display/device core (e.g., CM55).
 *              Requires: COMPONENT_RETARGET_IPC_DEVICE in Makefile.
 *
 * Copyright 2026 Infineon Technologies AG
 * SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

#include "retarget_ipc.h"

/* ARM memory barrier intrinsics */
#ifndef __DMB
#define __DMB() __asm volatile ("dmb sy" ::: "memory")
#endif

/*******************************************************************************
 * Module State
 ******************************************************************************/

/** Pointer to the shared memory structure (same address as host) */
static retarget_ipc_shared_t *const s_shared =
    (retarget_ipc_shared_t *)RETARGET_IPC_SHARED_ADDR;

static bool s_initialized = false;

/*******************************************************************************
 * Public API — DEVICE
 ******************************************************************************/

void retarget_ipc_device_init(void)
{
    /*
     * We don't write the buffer structure — the HOST owns initialization.
     * Just mark ourselves as ready to receive. The first call to
     * retarget_ipc_read() will check the magic value.
     */
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

    /* Check if host has initialized */
    if (s_shared->magic != RETARGET_IPC_MAGIC)
    {
        return 0;
    }

    /* Memory barrier — ensure we see the latest head value */
    __DMB();

    uint32_t tail = s_shared->tail;
    const uint32_t head = s_shared->head;  /* Read once (host may update) */
    uint32_t count = 0;

    while (tail != head && count < max_len)
    {
        buf[count] = s_shared->data[tail];
        tail = (tail + 1U) % RETARGET_IPC_BUFFER_SIZE;
        count++;
    }

    if (count > 0)
    {
        /* Memory barrier before publishing new tail */
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
        s_shared->dropped = 0;
        __DMB();
    }
    return dropped;
}
