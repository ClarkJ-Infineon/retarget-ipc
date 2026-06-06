/******************************************************************************
 * @file retarget_ipc.h
 * @brief Multi-core printf relay via shared memory ring buffer.
 *
 * @details
 * This library provides transparent cross-core printf routing using a
 * lock-free SPSC (Single-Producer Single-Consumer) ring buffer placed in
 * shared memory. It works with any stdio backend on the receiver side
 * (retarget-io for UART, retarget-lvgl for display, or custom).
 *
 * The library is **bidirectional** — either core can be HOST (sender) or
 * DEVICE (receiver). Use Makefile COMPONENTS to select the role:
 *
 * @par Direction 1: CM33 printf → CM55 backend
 * - CM33_NS Makefile: `COMPONENTS += RETARGET_IPC_HOST`
 * - CM55 Makefile:    `COMPONENTS += RETARGET_IPC_DEVICE`
 *
 * @par Direction 2: CM55 printf → CM33 backend
 * - CM55 Makefile:    `COMPONENTS += RETARGET_IPC_HOST`
 * - CM33_NS Makefile: `COMPONENTS += RETARGET_IPC_DEVICE`
 *
 * @par Architecture:
 * @code
 *   HOST core:   printf() → _write() → shared memory ring buffer
 *   DEVICE core: retarget_ipc_read() → local backend (UART/display/custom)
 * @endcode
 *
 * @par Thread Safety:
 * - HOST _write() uses FreeRTOS critical sections (safe from any task/ISR)
 * - DEVICE retarget_ipc_read() is task-safe (uses memory barriers only)
 * - Do NOT call printf() from ISR context (Newlib limitation)
 *
 * @par Memory Requirements:
 * - Shared buffer struct: ~4.1 KB in non-cacheable shared SOCMEM
 * - Code: ~500 bytes per side
 *
 * @note The shared memory region MUST be non-cacheable or use explicit cache
 *       maintenance. Default PSOC Edge E84 BSP configures SOCMEM shared region
 *       as non-cacheable via MPU.
 *
 * @target PSOC Edge E84 (dual-core CM33 + CM55, shared SOCMEM)
 *
 * @copyright Copyright 2025-2026 Clark Jarvis / Infineon Technologies AG
 * @license SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

#ifndef RETARGET_IPC_H
#define RETARGET_IPC_H

#include <stdint.h>
#include <stdbool.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** @defgroup retarget_ipc_config Configuration
 *  Compile-time configuration — override via Makefile DEFINES.
 *  @{ */

/**
 * @brief Size of the shared memory ring buffer in bytes.
 * @note Must be a power of 2 for efficient modulo via bitmask.
 */
#ifndef RETARGET_IPC_BUFFER_SIZE
#define RETARGET_IPC_BUFFER_SIZE        (4096U)
#endif

/**
 * @brief Base address of the shared memory region for the IPC ring buffer.
 *
 * Must be accessible by both cores and configured as non-cacheable.
 *
 * Default placement on PSOC Edge E84:
 * - SOCMEM base: 0x26000000
 * - Shared region (m33_m55_shared): offset 0x001C0000, size 256KB
 * - IPC buffer at end: 0x261C0000 + 0x3E000 = 0x261FE000
 *
 * @warning Ensure this address does not conflict with other shared memory
 *          users (e.g., mtb-ipc, WiFi firmware mailbox).
 */
#ifndef RETARGET_IPC_SHARED_ADDR
#define RETARGET_IPC_SHARED_ADDR        (0x261FE000UL)
#endif

/**
 * @brief Magic value written by HOST to signal buffer is initialized.
 * @details ASCII "IPCR" (0x49='I', 0x50='P', 0x43='C', 0x52='R')
 */
#define RETARGET_IPC_MAGIC              (0x49504352UL)

/** @} */ /* end of retarget_ipc_config */

/** @defgroup retarget_ipc_shared Shared Memory Layout
 *  Visible to both cores — placed at RETARGET_IPC_SHARED_ADDR.
 *  @{ */

/**
 * @brief SPSC ring buffer structure in shared memory.
 *
 * This struct is placed at RETARGET_IPC_SHARED_ADDR. The HOST core writes
 * (advances head), the DEVICE core reads (advances tail).
 *
 * Lock-free SPSC design:
 * - Only HOST writes `head` and `dropped`
 * - Only DEVICE writes `tail`
 * - ARM DMB (Data Memory Barrier) ensures cross-core visibility
 * - No mutex needed between cores
 *
 * @warning This struct MUST reside in non-cacheable memory. The default
 *          PSOC Edge E84 BSP maps SOCMEM shared region as Device-nGnRE
 *          (non-cacheable) via MPU.
 */
typedef struct
{
    volatile uint32_t magic;    /**< Must equal RETARGET_IPC_MAGIC when initialized */
    volatile uint32_t head;     /**< Write index [0..BUFFER_SIZE-1], owned by HOST */
    volatile uint32_t tail;     /**< Read index [0..BUFFER_SIZE-1], owned by DEVICE */
    volatile uint32_t dropped;  /**< Bytes dropped (buffer full), HOST increments */
    volatile uint8_t  data[RETARGET_IPC_BUFFER_SIZE]; /**< Ring buffer payload */
} retarget_ipc_shared_t;

/** @} */ /* end of retarget_ipc_shared */

/** @defgroup retarget_ipc_host HOST API (sender core)
 *  Compiled when COMPONENT_RETARGET_IPC_HOST is active.
 *  @{ */

/**
 * @brief Initialize the IPC host (sender) side.
 *
 * Zeros the shared memory ring buffer and writes the magic value to signal
 * readiness to the DEVICE core. Must be called before the FreeRTOS scheduler
 * starts (or early in main()).
 *
 * After this call, all printf() output is captured via a strong `_write()`
 * symbol and placed into the shared memory buffer.
 *
 * @pre  Shared memory region is accessible and non-cacheable.
 * @post _write() override is active; printf() routes to shared memory.
 */
void retarget_ipc_host_init(void);

#ifdef RETARGET_IPC_UART_TEE
#include "mtb_hal.h"

/**
 * @brief Enable UART tee (dual output) on the host core.
 *
 * When enabled, printf output goes to BOTH:
 * - Shared memory (for the device core to consume)
 * - The specified UART (for local serial debugging)
 *
 * @param[in] uart  Pointer to an initialized and enabled mtb_hal_uart_t.
 *                  Must remain valid for the lifetime of the application.
 *
 * @pre  UART must be initialized and enabled before calling this function.
 */
void retarget_ipc_host_set_uart(mtb_hal_uart_t *uart);
#endif

/** @} */ /* end of retarget_ipc_host */

/** @defgroup retarget_ipc_device DEVICE API (receiver core)
 *  Compiled when COMPONENT_RETARGET_IPC_DEVICE is active.
 *  @{ */

/**
 * @brief Initialize the IPC device (receiver) side.
 *
 * Marks the receiver as ready. Does NOT write to the shared buffer
 * (only HOST initializes it). The first call to retarget_ipc_read()
 * will check for the magic value.
 *
 * @note The HOST core must call retarget_ipc_host_init() before the
 *       DEVICE can successfully read data. Order is enforced by the
 *       magic value handshake.
 */
void retarget_ipc_device_init(void);

/**
 * @brief Read available data from the shared ring buffer.
 *
 * Copies up to @p max_len bytes into the output buffer. Non-blocking:
 * returns 0 immediately if no data is available or HOST hasn't initialized.
 *
 * @param[out] buf      Destination buffer (caller-allocated).
 * @param[in]  max_len  Maximum number of bytes to read.
 *
 * @return Number of bytes actually read (0 if empty or not initialized).
 *
 * @note Thread-safe via memory barriers. Can be called from any FreeRTOS task.
 * @note Do NOT call from ISR context (memory barriers may not be sufficient).
 */
uint32_t retarget_ipc_read(uint8_t *buf, uint32_t max_len);

/**
 * @brief Check if the IPC channel is active (HOST has initialized).
 *
 * @retval true   Shared memory magic is valid; data can be read.
 * @retval false  HOST has not yet initialized, or receiver not ready.
 */
bool retarget_ipc_is_active(void);

/**
 * @brief Get and clear the dropped byte counter.
 *
 * Returns the number of bytes the HOST dropped due to buffer-full
 * conditions since the last call to this function. Useful for diagnostics
 * and flow-control monitoring.
 *
 * @return Number of bytes dropped (atomically clears the counter).
 */
uint32_t retarget_ipc_get_dropped(void);

/** @} */ /* end of retarget_ipc_device */

#if defined(__cplusplus)
}
#endif

#endif /* RETARGET_IPC_H */
