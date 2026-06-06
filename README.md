# retarget-ipc — Multi-Core Printf Relay

Route `printf` output from one core to another via shared memory. Designed for PSOC Edge E84 dual-core applications where the application runs on CM33 but output is consumed on CM55 (via UART or display).

## Architecture

```
CM33_NS (HOST):  printf("hello")
    → _write()  [COMPONENT_RETARGET_IPC_HOST]
        → shared memory ring buffer (lock-free SPSC)
        → optional UART tee (local debugging)

CM55 (DEVICE):
    retarget_ipc_read()  [COMPONENT_RETARGET_IPC_DEVICE]
        → returns buffered data for local consumption
        → feed into retarget-io (UART) or retarget-lvgl (display)
```

## Use Cases

| HOST output | DEVICE backend | Result |
|---|---|---|
| `retarget-ipc` | `retarget-io` | CM33 printf → CM55 UART |
| `retarget-ipc` | `retarget-lvgl` | CM33 printf → CM55 display |
| `retarget-ipc` | Custom | CM33 printf → anything on CM55 |

## Quick Start

### 1. Add to Both Core Projects

**`proj_cm33_ns/deps/retarget-ipc.mtb`:**
```
https://github.com/ClarkJ-Infineon/retarget-ipc#main#$$ASSET_REPO$$/retarget-ipc/main
```

**`proj_cm55/deps/retarget-ipc.mtb`:**
```
https://github.com/ClarkJ-Infineon/retarget-ipc#main#$$ASSET_REPO$$/retarget-ipc/main
```

### 2. Configure Makefiles

**`proj_cm33_ns/Makefile`:**
```makefile
COMPONENTS += RETARGET_IPC_HOST

# Remove retarget-io (conflicts with _write override)
# Delete deps/retarget-io.mtb

# Optional: Enable UART tee for local CM33 debug output
DEFINES += RETARGET_IPC_UART_TEE

# Include path for the library header
INCLUDES += $(SEARCH_retarget-ipc)/include
```

**`proj_cm55/Makefile`:**
```makefile
COMPONENTS += RETARGET_IPC_DEVICE

# Include path for the library header
INCLUDES += $(SEARCH_retarget-ipc)/include
```

### 3. Initialize in Code

**CM33_NS `main.c`:**
```c
#include "retarget_ipc.h"

int main(void)
{
    cybsp_init();
    __enable_irq();

    /* Initialize IPC (sets up shared memory) */
    retarget_ipc_host_init();

    /* Optional: UART tee for local serial output */
    #ifdef RETARGET_IPC_UART_TEE
    // ... init UART ...
    retarget_ipc_host_set_uart(&my_uart);
    #endif

    /* Now printf goes to shared memory → CM55 */
    printf("Hello from CM33!\n");

    vTaskStartScheduler();
}
```

**CM55 — Using with retarget-lvgl:**
```c
#include "retarget_ipc.h"

void gfx_task(void *arg)
{
    retarget_ipc_device_init();

    // ... LVGL init ...
    // retarget-lvgl automatically calls retarget_ipc_read() if available
    retarget_lvgl_init(lv_screen_active());

    while (1) {
        lv_timer_handler();
        vTaskDelay(5);
    }
}
```

**CM55 — Using with retarget-io (UART relay):**
```c
#include "retarget_ipc.h"
#include "cy_retarget_io.h"

void relay_task(void *arg)
{
    uint8_t buf[256];
    retarget_ipc_device_init();

    while (1) {
        uint32_t n = retarget_ipc_read(buf, sizeof(buf));
        if (n > 0) {
            /* Send to UART via retarget-io's uart object */
            for (uint32_t i = 0; i < n; i++) {
                cyhal_uart_putc(&cy_retarget_io_uart_obj, buf[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

## API Reference

### HOST Side (COMPONENT_RETARGET_IPC_HOST)

| Function | Description |
|----------|-------------|
| `retarget_ipc_host_init()` | Initialize shared memory buffer. Call in `main()` before scheduler. |
| `retarget_ipc_host_set_uart(uart)` | Enable UART tee (requires `RETARGET_IPC_UART_TEE` define) |

The library also provides a strong `_write()` that captures all printf output.

### DEVICE Side (COMPONENT_RETARGET_IPC_DEVICE)

| Function | Description |
|----------|-------------|
| `retarget_ipc_device_init()` | Prepare receiver. Call before reading. |
| `retarget_ipc_read(buf, max_len)` | Read available data. Non-blocking, returns 0 if empty. |
| `retarget_ipc_is_active()` | Check if host has initialized the channel. |
| `retarget_ipc_get_dropped()` | Get and clear dropped byte count. |

## Configuration

| Define | Default | Description |
|--------|---------|-------------|
| `RETARGET_IPC_BUFFER_SIZE` | 4096 | Ring buffer size (power of 2) |
| `RETARGET_IPC_SHARED_ADDR` | `0x261FE000` | Shared memory address |
| `RETARGET_IPC_UART_TEE` | (not defined) | Enable UART passthrough on host |

## Memory Requirements

| Resource | Size | Location |
|----------|------|----------|
| Shared ring buffer struct | ~4.1 KB | Shared SOCMEM (non-cacheable) |
| Host code | ~400 bytes | CM33_NS Flash |
| Device code | ~300 bytes | CM55 Flash |

## Prerequisites

- PSOC Edge E84 dual-core project (CM33 + CM55)
- Shared memory region accessible by both cores (default BSP provides this)
- FreeRTOS on at least one core (for task scheduling)
- `retarget-io` must be **removed** from the HOST core project (conflicting `_write()`)

## How It Works

The shared memory ring buffer uses a **lock-free Single-Producer Single-Consumer (SPSC)** design:
- Only the HOST writes `head` / Only the DEVICE writes `tail`
- ARM `DMB` (Data Memory Barrier) instructions ensure ordering
- No mutex needed — safe across cores without synchronization primitives
- Non-blocking: if buffer is full, HOST drops bytes and increments `dropped` counter

## Supported Platforms

| Platform | HOST Core | DEVICE Core | Status |
|----------|-----------|-------------|--------|
| PSOC Edge E84 | CM33_NS | CM55 | ✅ Primary target |
| PSOC Edge E84 | CM33_S | CM55 | Should work |

## License

Apache-2.0
