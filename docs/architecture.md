# isotp-cpp Architecture

C++ wrapper for [isotp-c](https://github.com/SimonCahill/isotp-c) — ISO 15765-2 (ISO-TP) protocol library.

## Design Goals

- Zero-overhead abstraction over the C library
- No virtual dispatch — CRTP for compile-time polymorphism
- C++17 compatible
- RAII resource management (link lifecycle, buffers)
- Non-template `Link` class — one compilation unit, predictable code size

## File Layout

```
isotp-cpp/
├── include/isotp/
│   ├── isotp.hpp          # Umbrella include
│   ├── platform.hpp       # CRTP PlatformBase<Derived> + PlatformConfig
│   ├── link.hpp           # Non-template Link (Pimpl)
│   └── types.hpp          # ErrorCode enum
├── src/
│   ├── platform.cpp       # extern "C" trampolines + global config
│   └── link.cpp           # Link::Impl + method implementations
├── CMakeLists.txt
├── docs/
│   └── architecture.md    # This file
└── isotp-c/               # Git submodule (upstream C library)
```

## Callback Routing Problem

The isotp-c library requires three global `extern "C"` functions:

```c
int      isotp_user_send_can(uint32_t id, const uint8_t* data, uint8_t size);
uint32_t isotp_user_get_us(void);
void     isotp_user_debug(const char* msg, ...);
```

These are global functions with no context parameter. The C++ wrapper must route calls to the user's platform-specific implementation.

### Solution: Global PlatformConfig + Function Pointers

```cpp
struct PlatformConfig {
    void* instance;    // pointer to user's Derived platform object
    int  (*send_can)(uint32_t arb_id, const uint8_t* data,
                     uint8_t size, uint8_t flags, void* ctx);
    uint32_t (*get_time_us)(void* ctx);
    void (*debug)(const char* msg, void* ctx);
};
```

All three callbacks route through a single global `PlatformConfig*`. The `void* instance` carries a pointer to the user's platform object. The `extern "C"` trampolines call the function pointers, which forward to the user's implementation.

No `ISO_TP_USER_SEND_CAN_ARG` needed. No virtual dispatch.

## CRTP Platform Pattern

Users inherit from `PlatformBase<Derived>` and implement the `_impl` methods:

```cpp
template <typename Derived>
class PlatformBase {
public:
    int send_can(uint32_t arb_id, const uint8_t* data,
                 uint8_t size, uint8_t flags) {
        return static_cast<Derived*>(this)->send_can_impl(arb_id, data, size, flags);
    }

    uint32_t get_time_us() {
        return static_cast<Derived*>(this)->get_time_us_impl();
    }

    void debug(const char* fmt, ...) {
        static_cast<Derived*>(this)->debug_impl(fmt);
    }

protected:
    ~PlatformBase() = default;
};
```

The CRTP base provides a convenient object-oriented API. The compiler resolves `static_cast<Derived*>(this)->send_can_impl(...)` at compile time — zero overhead, no vtable.

### Why Both CRTP and Function Pointers?

| Mechanism | Purpose |
|-----------|---------|
| CRTP `PlatformBase<Derived>` | User-facing API: `platform.send_can(...)` compiles to a direct call to `send_can_impl()`. |
| Function pointers in `PlatformConfig` | Routing `extern "C"` callbacks: trampolines don't know the `Derived` type, they call through function pointers. |

CRTP gives users a clean OO interface. Function pointers solve type erasure for the C callbacks. They complement each other.

## Link Class

Non-template, Pimpl pattern, move-only:

```cpp
class Link {
public:
    Link(uint32_t send_arb_id, uint32_t rx_arb_id,
         uint32_t tx_buf_size = 4096, uint32_t rx_buf_size = 4096);
    ~Link();

    Link(Link&&) noexcept;
    Link& operator=(Link&&) noexcept;
    Link(const Link&) = delete;

    int  send(const uint8_t* payload, uint32_t size);
    int  send_with_id(uint32_t arb_id, const uint8_t* payload, uint32_t size);
    void on_can_message(const uint8_t* data, uint8_t len);
    int  receive(uint8_t* buf, uint32_t buf_size, uint32_t* out_size);
    void poll();
    int  set_tx_dl(uint8_t dl);
    uint8_t get_tx_dl() const;
    bool is_sending() const;
    bool is_receiving() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

### Pimpl + Move Semantics

`Link::Impl` holds the `IsoTpLink` struct and the TX/RX buffers:

```cpp
struct Link::Impl {
    IsoTpLink link;
    std::vector<uint8_t> tx_buf;
    std::vector<uint8_t> rx_buf;
};
```

`unique_ptr<Impl>` ensures the `IsoTpLink*` address stays stable across moves. Trampolines store a pointer to `Impl`, which is never invalidated.

### Method Mapping

| Link method | C function |
|-------------|------------|
| Constructor | `isotp_init_link()` |
| Destructor | `isotp_destroy_link()` |
| `send()` | `isotp_send()` |
| `send_with_id()` | `isotp_send_with_id()` |
| `on_can_message()` | `isotp_on_can_message()` |
| `receive()` | `isotp_receive()` |
| `poll()` | `isotp_poll()` |
| `set_tx_dl()` | `isotp_set_tx_dl()` |
| `get_tx_dl()` | `isotp_get_tx_dl()` |

## Callback Flow

```
User calls:  platform.send_can(id, data, size, flags)
                    │
                    ▼
         PlatformBase<Derived>::send_can()
                    │
                    static_cast<Derived*>(this)->send_can_impl(...)
                    │
                    ▼ (compiler inlines)

         ┌─── Later, when C library calls isotp_user_send_can ───┐
         │                                                        │
isotp_user_send_can(id, data, size, flags)  ← extern "C"         │
         │                                                        │
         ▼                                                        │
trampoline: active_config->send_can(data, size, flags, ctx)      │
         │                                                        │
         ▼                                                        │
MyPlatform::send_can_impl()  ← same function, called via ptr ───┘
```

## Usage Example

```cpp
#include <isotp/isotp.hpp>

class MyPlatform : public isotp::PlatformBase<MyPlatform> {
public:
    int send_can_impl(uint32_t id, const uint8_t* data,
                      uint8_t size, uint8_t flags) {
        return my_can.transmit(id, data, size, flags);
    }

    uint32_t get_time_us_impl() {
        return my_timer.now_microseconds();
    }

    void debug_impl(const char* msg) {
        my_logger.log(msg);
    }

private:
    MyCanDriver my_can;
    MyTimer my_timer;
    MyLogger my_logger;
};

// Trampolines for PlatformConfig
static int send_can_trampoline(uint32_t id, const uint8_t* data,
                                uint8_t size, uint8_t flags, void* ctx) {
    return static_cast<MyPlatform*>(ctx)->send_can_impl(id, data, size, flags);
}

static uint32_t get_us_trampoline(void* ctx) {
    return static_cast<MyPlatform*>(ctx)->get_time_us_impl();
}

static void debug_trampoline(const char* msg, void* ctx) {
    static_cast<MyPlatform*>(ctx)->debug_impl(msg);
}

int main() {
    MyPlatform platform;
    isotp::set_platform_config({
        .instance = &platform,
        .send_can = send_can_trampoline,
        .get_time_us = get_us_trampoline,
        .debug = debug_trampoline
    });

    isotp::Link tx_link(0x123, 0x456);

    uint8_t payload[] = {0x01, 0x02, 0x03};
    tx_link.send(payload, sizeof(payload));

    // In main loop:
    tx_link.poll();

    // Receive
    isotp::Link rx_link(0x789, 0x012);
    // ... in CAN receive callback:
    rx_link.on_can_message(frame_data, frame_len);
    // ... after on_can_message:
    uint8_t buf[256];
    uint32_t received;
    if (rx_link.receive(buf, sizeof(buf), &received) == 0) {
        // process buf[0..received-1]
    }
}
```

## CMake Integration

```cmake
option(ISOTP_CPP_ENABLE_STREAMING "Enable chunked receive" OFF)
option(ISOTP_CPP_CAN_FD "Enable CAN FD (max frame size 64)" OFF)
option(ISOTP_CPP_BUILD_TESTS "Build tests" OFF)
```

The wrapper's `CMakeLists.txt` adds `isotp-c/` as a subdirectory and builds both the C library and C++ wrapper as a single library target. Key CMake options are forwarded to isotp-c automatically.

## Error Handling

Int return codes matching the C API:

| Code | Value | Meaning |
|------|-------|---------|
| `ErrorCode::Ok` | 0 | Success |
| `ErrorCode::Error` | -1 | General error |
| `ErrorCode::InProgress` | -2 | TX already in progress |
| `ErrorCode::Overflow` | -3 | Payload too large for buffer |
| `ErrorCode::WrongSn` | -4 | Unexpected sequence number |
| `ErrorCode::NoData` | -5 | No complete message yet |
| `ErrorCode::Timeout` | -6 | Protocol timeout |
| `ErrorCode::Length` | -7 | Invalid frame length |
| `ErrorCode::NoSpace` | -8 | CAN driver busy, retry later |

## Compile-Time Features

The wrapper respects isotp-c's compile-time configuration:

| Feature | CMake option | Effect |
|---------|-------------|--------|
| CAN FD | `ISOTP_CPP_CAN_FD` | Sets `isotpc_MAX_CAN_FRAME_SIZE=64` |
| Streaming | `ISOTP_CPP_ENABLE_STREAMING` | Enables chunked receive for messages larger than buffer |
| Frame padding | `isotpc_PAD_CAN_FRAMES` | Pads short frames to CAN_DL |
| CAN send flags | `isotpc_ENABLE_CAN_SEND_FLAGS` | Adds `flags` parameter to `send_can` |
| CAN FD BRS | `isotpc_ENABLE_CAN_FD_BRS` | Bit Rate Switch for CAN FD |
