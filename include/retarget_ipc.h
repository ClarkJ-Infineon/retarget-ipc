/******************************************************************************
 * File: retarget_ipc.h
 *
 * Description: Public interface for retarget-ipc library.
 *              Provides multi-core printf relay via shared memory ring buffer.
 *              Works with any stdio backend on the receiver (retarget-io,
 *              retarget-lvgl, or custom).
 *
 * Architecture:
 *   HOST core (e.g., CM33_NS): printf → _write() → shared memory
 *   DEVICE core (e.g., CM55):  retarget_ipc_read() → local backend
 *
 * Target: PSOC Edge E84 (dual-core CM33 + CM55, shared SOCMEM)
 *
 * Copyright 2026 Infineon Technologies AG
 * SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

#ifndef RETARGET_IPC_H
#define RETARGET_IPC_H

#include <stdint.h>
#include <stdbool.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*******************************************************************************
 * Configuration — Override via Makefile DEFINES
 ******************************************************************************/

/** Size of the shared memory ring buffer (must be power of 2) */
#ifndef RETARGET_IPC_BUFFER_SIZE
#define RETARGET_IPC_BUFFER_SIZE        (4096U)
#endif

/**
 * Base address of the shared memory region used for the IPC ring buffer.
 * Must be accessible by both cores (non-cacheable recommended).
 *
 * Default: End of m33_m55_shared region in SOCMEM on PSOC Edge E84
 * SOCMEM base: 0x26000000, shared region at offset 0x001C0000 (256KB)
 * We use the last 8KB: 0x261C0000 + 0x3E000 = 0x261FE000
 */
#ifndef RETARGET_IPC_SHARED_ADDR
#define RETARGET_IPC_SHARED_ADDR        (0x261FE000UL)
#endif

/** Magic value to verify shared memory is initialized */
#define RETARGET_IPC_MAGIC              (0x49504352UL)  /* "IPCR" in ASCII */

/*******************************************************************************
 * Shared Memory Layout (visible to both cores)
 ******************************************************************************/

/**
 * @brief Single-Producer Single-Consumer (SPSC) ring buffer in shared memory.
 *
 * This struct is placed at RETARGET_IPC_SHARED_ADDR. The HOST core writes
 * (advances head), the DEVICE core reads (advances tail). Lock-free SPSC
 * requires no mutex — only memory barriers for ARM multi-core coherency.
 *
 * IMPORTANT: This struct MUST be placed in non-cacheable memory, OR both
 * cores must use cache maintenance operations. The default BSP configuration
 * for PSOC Edge E84 maps the shared SOCMEM region as non-cacheable via MPU.
 */
typedef struct
{
    volatile uint32_t magic;                        /**< Must be RETARGET_IPC_MAGIC when initialized */
    volatile uint32_t head;                         /**< Write index (owned by HOST) */
    volatile uint32_t tail;                         /**< Read index (owned by DEVICE) */
    volatile uint32_t dropped;                      /**< Bytes dropped due to full buffer (HOST increments) */
    volatile uint8_t  data[RETARGET_IPC_BUFFER_SIZE]; /**< Ring buffer payload */
} retarget_ipc_shared_t;

/*******************************************************************************
 * HOST API (sender core — e.g., CM33_NS)
 * Compiled when COMPONENT_RETARGET_IPC_HOST is active
 ******************************************************************************/

/**
 * @brief Initialize the IPC host (sender) side.
 *
 * Initializes the shared memory ring buffer structure. Must be called
 * before the FreeRTOS scheduler starts (or early in main()).
 *
 * After this call, all printf() output is captured via _write() and
 * placed into the shared memory buffer for the device core to consume.
 */
void retarget_ipc_host_init(void);

#ifdef RETARGET_IPC_UART_TEE
#include "mtb_hal.h"

/**
 * @brief Enable UART tee on the host core.
 *
 * When enabled, printf output goes to both shared memory (for the device
 * core) AND the specified UART (for local serial debugging).
 *
 * @param uart  Pointer to initialized and enabled mtb_hal_uart_t
 */
void retarget_ipc_host_set_uart(mtb_hal_uart_t *uart);
#endif

/*******************************************************************************
 * DEVICE API (receiver core — e.g., CM55)
 * Compiled when COMPONENT_RETARGET_IPC_DEVICE is active
 ******************************************************************************/

/**
 * @brief Initialize the IPC device (receiver) side.
 *
 * Prepares the receiver to read from the shared memory ring buffer.
 * Does NOT initialize the buffer itself (that's done by the host).
 * Waits for the magic value to appear (host must init first).
 */
void retarget_ipc_device_init(void);

/**
 * @brief Read available data from the host core's shared buffer.
 *
 * Copies up to max_len bytes from the shared ring buffer into the
 * output buffer. Non-blocking — returns 0 if no data available.
 *
 * Thread-safe: uses memory barriers. Can be called from any task.
 *
 * @param[out] buf      Destination buffer
 * @param[in]  max_len  Maximum bytes to read
 * @return Number of bytes read (0 if empty or not initialized)
 */
uint32_t retarget_ipc_read(uint8_t *buf, uint32_t max_len);

/**
 * @brief Check if the IPC channel is initialized and active.
 *
 * @return true if the shared memory magic is valid (host has initialized)
 */
bool retarget_ipc_is_active(void);

/**
 * @brief Get the number of bytes dropped by the host due to buffer overflow.
 *
 * Returns and clears the dropped counter. Useful for diagnostics.
 *
 * @return Number of bytes dropped since last call
 */
uint32_t retarget_ipc_get_dropped(void);

#if defined(__cplusplus)
}
#endif

#endif /* RETARGET_IPC_H */
