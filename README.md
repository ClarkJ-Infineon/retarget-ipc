# retarget-ipc — Multi-Core Printf Relay

Route `printf` output from one core to another via shared memory. Designed for PSOC Edge E84 dual-core applications. **Bidirectional** — either core can be the sender (HOST) or receiver (DEVICE).

## Architecture

```
HOST core:  printf("hello")
    → _write()  [COMPONENT_RETARGET_IPC_HOST]
        → shared memory ring buffer (lock-free SPSC)
        → optional UART tee (local debugging)

DEVICE core:
    retarget_ipc_read()  [COMPONENT_RETARGET_IPC_DEVICE]
        → returns buffered data for local consumption
        → feed into retarget-io (UART) or retarget-lvgl (display)
```

## Use Cases

| Direction | HOST (sender) | DEVICE (receiver) | Result |
|---|---|---|---|
| CM33→CM55 | CM33_NS | CM55 + retarget-lvgl | App printf → display |
| CM33→CM55 | CM33_NS | CM55 + retarget-io | App printf → CM55 UART |
| CM55→CM33 | CM55 | CM33_NS + retarget-io | App printf → CM33 UART |
| CM55→CM33 | CM55 | CM33_NS + custom | App printf → any CM33 backend |

## Quick Start — Direction 1: CM33 → CM55 (Display)

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

    retarget_ipc_host_init();

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
    retarget_lvgl_init(lv_screen_active());

    while (1) {
        lv_timer_handler();
        vTaskDelay(5);
    }
}
```

---

## Quick Start — Direction 2: CM55 → CM33 (UART)

For applications where the main logic runs on CM55 but UART is on CM33.

### CM55 Makefile:
```makefile
COMPONENTS += RETARGET_IPC_HOST
INCLUDES += $(SEARCH_retarget-ipc)/include
# Remove retarget-io from CM55 (conflicts with _write)
```

### CM33_NS Makefile:
```makefile
COMPONENTS += RETARGET_IPC_DEVICE
INCLUDES += $(SEARCH_retarget-ipc)/include
# Keep retarget-io for CM33's own UART output (no conflict — DEVICE has no _write)
```

### CM55 `main.c`:
```c
#include "retarget_ipc.h"

int main(void)
{
    cybsp_init();
    __enable_irq();
    retarget_ipc_host_init();

    printf("Hello from CM55!\n");  /* → shared memory → CM33 UART */
    vTaskStartScheduler();
}
```

### CM33_NS `main.c` (relay task):
```c
#include "retarget_ipc.h"
#include "cy_retarget_io.h"

void ipc_relay_task(void *arg)
{
    uint8_t buf[256];
    retarget_ipc_device_init();

    while (1) {
        uint32_t n = retarget_ipc_read(buf, sizeof(buf));
        if (n > 0) {
            for (uint32_t i = 0; i < n; i++) {
                cyhal_uart_putc(&cy_retarget_io_uart_obj, buf[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

---

## API Reference

### HOST Side (COMPONENT_RETARGET_IPC_HOST)

| Function | Description |
|----------|-------------|
| `retarget_ipc_host_init()` | Initialize shared memory buffer. Call before scheduler. |
| `retarget_ipc_host_set_uart(uart)` | Enable UART tee (requires `RETARGET_IPC_UART_TEE` define) |

The library provides a strong `_write()` that captures all printf output.

### DEVICE Side (COMPONENT_RETARGET_IPC_DEVICE)

| Function | Description |
|----------|-------------|
| `retarget_ipc_device_init()` | Prepare receiver. Call before reading. |
| `retarget_ipc_read(buf, max_len)` | Read available data. Non-blocking, returns 0 if empty. |
| `retarget_ipc_is_active()` | Check if HOST has initialized the channel. |
| `retarget_ipc_get_dropped()` | Get and clear dropped byte count. |

## Configuration

| Define | Default | Description |
|--------|---------|-------------|
| `RETARGET_IPC_BUFFER_SIZE` | 4096 | Ring buffer size (power of 2) |
| `RETARGET_IPC_SHARED_ADDR` | `0x261FE000` | Shared memory address |
| `RETARGET_IPC_UART_TEE` | (not defined) | Enable UART passthrough on HOST |

## How It Works

The shared memory ring buffer uses a **lock-free Single-Producer Single-Consumer (SPSC)** design:
- Only the HOST writes `head` / Only the DEVICE writes `tail`
- ARM `DMB` (Data Memory Barrier) instructions ensure cross-core visibility
- FreeRTOS critical sections protect HOST writes from concurrent tasks
- No mutex needed between cores — safe without inter-core synchronization
- Non-blocking: if buffer is full, HOST drops bytes and increments `dropped` counter

## Memory Requirements

| Resource | Size | Location |
|----------|------|----------|
| Shared ring buffer struct | ~4.1 KB | Shared SOCMEM (non-cacheable) |
| Host code | ~500 bytes | HOST core Flash |
| Device code | ~400 bytes | DEVICE core Flash |

## Prerequisites

- PSOC Edge E84 dual-core project (CM33 + CM55)
- Shared memory region accessible by both cores (non-cacheable)
- FreeRTOS on HOST core (for critical sections in `_write()`)
- `retarget-io` must be **removed** from the HOST core project (conflicting `_write()`)
- `retarget-io` **may remain** on the DEVICE core (no conflict)

## Supported Platforms

| Platform | Direction | Status |
|----------|-----------|--------|
| PSOC Edge E84 | CM33_NS → CM55 | ✅ Primary target |
| PSOC Edge E84 | CM55 → CM33_NS | ✅ Supported |
| PSOC Edge E84 | CM33_S → CM55 | Should work |

## License

Apache-2.0
