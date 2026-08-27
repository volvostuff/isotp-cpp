# isotp-cpp Architecture

C++ wrapper for [isotp-c](https://github.com/SimonCahill/isotp-c) — ISO 15765-2 (ISO-TP) protocol library.

## Design Goals

- Zero-overhead abstraction over the C library
- No virtual dispatch — CRTP for compile-time polymorphism
- C++17 compatible
- RAII resource management (link lifecycle, buffers)
- Compile-time lock policy — zero overhead when locking disabled (default)
- Non-template Pimpl for `Link` internals — predictable code size per instantiation

## File Layout

```
isotp-cpp/
├── include/isotp/
│   ├── isotp.hpp          # Umbrella include
│   ├── platform.hpp       # CRTP PlatformBase<Derived> + PlatformConfig
│   ├── link.hpp           # Link<LockPolicy> (Pimpl, template)
│   └── types.hpp          # ErrorCode enum, NoLock, StdMutexLock, LockGuard
├── src/
│   ├── platform.cpp       # extern "C" trampolines + global config
│   └── link.cpp           # Link::Impl + method implementations + explicit instantiations
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
                     uint8_t size, uint8_t flags, void* link_arg, void* ctx);
    uint32_t (*get_time_us)(void* ctx);
    void (*debug)(const char* msg, va_list args, void* ctx);
};
```

All three callbacks route through a single global `PlatformConfig*`. The `void* instance` carries a pointer to the user's platform object. The `extern "C"` trampolines call the function pointers, which forward to the user's implementation.

The `send_can` function pointer receives **two** context pointers:
- `link_arg` — per-link context from `IsoTpLink::user_send_can_arg` (set via `Link::set_user_send_can_arg()`)
- `ctx` — global platform instance from `PlatformConfig::instance`

This enables multi-bus setups: each `Link` points to its own CAN driver, while sharing one global platform.

`ISO_TP_USER_SEND_CAN_ARG` is **required** — each `Link` passes its `user_send_can_arg` as `void* arg` in the `send_can` trampoline, enabling per-link context (e.g., different CAN buses).

## CRTP Platform Pattern

Users inherit from `PlatformBase<Derived>` and implement the `_impl` methods:

```cpp
template <typename Derived>
class PlatformBase {
public:
    int send_can(uint32_t arb_id, const uint8_t* data,
                 uint8_t size, uint8_t flags, void* link_arg) {
        return static_cast<Derived*>(this)->send_can_impl(arb_id, data, size, flags, link_arg);
    }

    uint32_t get_time_us() {
        return static_cast<Derived*>(this)->get_time_us_impl();
    }

    void debug(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        static_cast<Derived*>(this)->debug_impl(fmt, args);
        va_end(args);
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

Non-template Pimpl, move-only. Parameterized by lock policy:

```cpp
template <typename LockPolicy = NoLock>
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

    // Per-link context for multi-bus routing
    void set_user_send_can_arg(void* arg);
    void* get_user_send_can_arg() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    LockPolicy lock_;
};

// Convenience alias
using LockedLink = Link<StdMutexLock>;
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
| `set_user_send_can_arg()` | `link->user_send_can_arg = arg` |
| `get_user_send_can_arg()` | `link->user_send_can_arg` |

## Callback Flow

### send_can (multi-bus routing)

```
User sets per-link context:
    link_a.set_user_send_can_arg(&can_bus_a);
    link_b.set_user_send_can_arg(&can_bus_b);

C library calls:
    isotp_user_send_can(id, data, size, flags, arg=can_bus_a)
         │
         ▼
    trampoline: active_config->send_can(id, data, size, flags,
                                         link_arg=can_bus_a, ctx=platform)
         │
         ▼
    MyPlatform::send_can_impl(id, data, size, flags, link_arg)
         │
         ├── link_arg == &can_bus_a → send via CAN bus A
         └── link_arg == &can_bus_b → send via CAN bus B
```

### get_us / debug (global routing)

```
isotp_user_get_us()
    → active_config->get_time_us(active_config->instance)

isotp_user_debug(fmt, ...)
    → active_config->debug(fmt, args, active_config->instance)
```

## Usage Example

### Single Bus

```cpp
#include <isotp/isotp.hpp>

class MyPlatform : public isotp::PlatformBase<MyPlatform> {
public:
    int send_can_impl(uint32_t id, const uint8_t* data,
                      uint8_t size, uint8_t flags, void* /*link_arg*/) {
        return my_can.transmit(id, data, size, flags);
    }

    uint32_t get_time_us_impl() {
        return my_timer.now_microseconds();
    }

    void debug_impl(const char* fmt, va_list args) {
        char buf[128];
        vsnprintf(buf, sizeof(buf), fmt, args);
        my_logger.log(buf);
    }

private:
    MyCanDriver my_can;
    MyTimer my_timer;
    MyLogger my_logger;
};

ISOTP_MAKE_PLATFORM_TRAMPOLINES(MyPlatform)

int main() {
    MyPlatform platform;
    isotp::set_platform_config(isotp::make_platform_config(platform));

    isotp::Link<> link(0x123, 0x456);  // NoLock (default)
    uint8_t payload[] = {0x01, 0x02, 0x03};
    link.send(payload, sizeof(payload));
    link.poll();
}
```

### Multi-Bus (Two CAN Interfaces)

```cpp
#include <isotp/isotp.hpp>

// Two CAN drivers
CanDriver can_a(CAN_INTERFACE_0);
CanDriver can_b(CAN_INTERFACE_1);

class MultiBusPlatform : public isotp::PlatformBase<MultiBusPlatform> {
public:
    int send_can_impl(uint32_t id, const uint8_t* data,
                      uint8_t size, uint8_t flags, void* link_arg) {
        // link_arg = which CAN bus to use
        auto* driver = static_cast<CanDriver*>(link_arg);
        return driver->transmit(id, data, size, flags);
    }

    uint32_t get_time_us_impl() { return timer.now_microseconds(); }
    void debug_impl(const char* fmt, va_list args) {
        char buf[128]; vsnprintf(buf, sizeof(buf), fmt, args); logger.log(buf);
    }

private:
    MyTimer timer;
    MyLogger logger;
};

ISOTP_MAKE_PLATFORM_TRAMPOLINES(MultiBusPlatform)

int main() {
    MultiBusPlatform platform;
    isotp::set_platform_config(isotp::make_platform_config(platform));

    // Link A → CAN bus A
    isotp::Link<> link_a(0x100, 0x101);
    link_a.set_user_send_can_arg(&can_a);

    // Link B → CAN bus B
    isotp::Link<> link_b(0x200, 0x201);
    link_b.set_user_send_can_arg(&can_b);

    // Each link sends via its own CAN bus
    uint8_t payload[] = {0x01};
    link_a.send(payload, 1);  // → can_a.transmit(...)
    link_b.send(payload, 1);  // → can_b.transmit(...)
}
```

### Multi-Threaded (with locking)

```cpp
// Use LockedLink (Link<StdMutexLock>) for thread-safe access
isotp::LockedLink link(0x123, 0x456);

// Thread 1: main loop
link.poll();

// Thread 2: CAN receive callback
link.on_can_message(frame_data, frame_len);

// Both calls are serialized by the internal std::mutex
```

### Trampoline Macro

Writing trampoline functions manually is error-prone. The `ISOTP_MAKE_PLATFORM_TRAMPOLINES` macro generates all three trampolines automatically:

```cpp
// Expands to:
//   static int  send_can_trampoline(uint32_t, const uint8_t*, uint8_t, uint8_t, void*, void*);
//   static uint32_t get_us_trampoline(void*);
//   static void debug_trampoline(const char*, va_list, void*);
```

Additionally, `make_platform_config(Platform&)` constructs a `PlatformConfig` with the generated trampolines:

```cpp
template <typename Derived>
PlatformConfig make_platform_config(Derived& instance) {
    return PlatformConfig {
        .instance = &instance,
        .send_can = send_can_trampoline<Derived>,
        .get_time_us = get_us_trampoline<Derived>,
        .debug = debug_trampoline<Derived>
    };
}
```

This eliminates all manual trampoline boilerplate.

## CMake Integration

```cmake
option(ISOTP_CPP_ENABLE_STREAMING "Enable chunked receive" OFF)
option(ISOTP_CPP_CAN_FD "Enable CAN FD (max frame size 64)" OFF)
option(ISOTP_CPP_BUILD_TESTS "Build tests" OFF)
```

The wrapper's `CMakeLists.txt` adds `isotp-c/` as a subdirectory and builds both the C library and C++ wrapper as a single library target. Key CMake options are forwarded to isotp-c automatically.

**Required**: `ISO_TP_USER_SEND_CAN_ARG` is always enabled — it enables per-link context for multi-bus routing via `Link::set_user_send_can_arg()`.

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

## Thread Safety

`Link` is a template class parameterized by a **lock policy**:

```cpp
template <typename LockPolicy = NoLock>
class Link { /* ... */ };

using LockedLink = Link<StdMutexLock>;
```

The default is `NoLock` — zero overhead, intended for single-threaded embedded systems.

### Lock Policies

| Policy | Description | Use case |
|--------|-------------|----------|
| `NoLock` | No synchronization. `lock()`/`unlock()` are empty. | Embedded, single-threaded main loop |
| `StdMutexLock` | Wraps `std::mutex`. | Desktop, multi-threaded applications |
| Custom | User-defined type with `lock()` and `unlock()`. | RTOS, bare-metal with interrupt control |

### Built-in Policies

```cpp
struct NoLock {
    void lock() {}
    void unlock() {}
};

struct StdMutexLock {
    std::mutex mtx;
    void lock() { mtx.lock(); }
    void unlock() { mtx.unlock(); }
};
```

### Custom Policy Example (bare-metal interrupt lock)

```cpp
struct IrqLock {
    void lock() { __disable_irq(); }
    void unlock() { __enable_irq(); }
};

isotp::Link<IrqLock> link(0x123, 0x456);
```

### Custom Policy Example (spinlock)

```cpp
struct Spinlock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    void lock() { while (flag.test_and_set(std::memory_order_acquire)); }
    void unlock() { flag.clear(std::memory_order_release); }
};

isotp::Link<Spinlock> link(0x123, 0x456);
```

### What Gets Protected

Every method that mutates `IsoTpLink` state acquires the lock via `LockGuard`:

| Method | Mutates state | Locked |
|--------|---------------|--------|
| `send()` | TX state | Yes |
| `send_with_id()` | TX state | Yes |
| `on_can_message()` | RX + TX state | Yes |
| `receive()` | RX state | Yes |
| `poll()` | TX + RX state | Yes |
| `set_tx_dl()` | tx_dl field | Yes |
| `set_user_send_can_arg()` | link field | Yes |
| `get_tx_dl()` | read-only | No |
| `is_sending()` | read-only | No |
| `is_receiving()` | read-only | No |
| `get_user_send_can_arg()` | read-only | No |

### Concurrency Rules

- **Same link**: all mutating calls must be serialized. The lock policy handles this.
- **Different links**: safe to call concurrently. The C library's state is per-`IsoTpLink` with no shared mutable globals.
- **User shims** (`isotp_user_send_can`, `isotp_user_get_us`, `isotp_user_debug`): must be thread-safe by the user if called from multiple threads.

### Typical Configurations

**Embedded single-threaded (default)**:
```cpp
isotp::Link<> link(0x123, 0x456);  // NoLock — zero overhead
```

**RTOS with mutex**:
```cpp
isotp::LockedLink link(0x123, 0x456);  // StdMutexLock
```

**Bare-metal with interrupts**:
```cpp
isotp::Link<IrqLock> link(0x123, 0x456);  // disables IRQ during operations
```

## Known Limitations

### Debug Varargs Handling

The `debug` callback receives a `va_list` (not a pre-formatted string). The wrapper's `PlatformBase::debug()` correctly forwards the `va_list` to `debug_impl(fmt, args)`. The user must use `vsnprintf` or similar to format the message:

```cpp
void debug_impl(const char* fmt, va_list args) {
    char buf[128];
    vsnprintf(buf, sizeof(buf), fmt, args);
    my_logger.log(buf);
}
```

This differs from the C library's `isotp_user_debug(const char* message, ...)` which receives raw varargs. The wrapper bridges the gap by capturing `va_list` in `PlatformBase::debug()`.

### Single Global Platform for get_us / debug

While `send_can` supports per-link routing via `user_send_can_arg`, `get_time_us` and `debug` are global callbacks without a context parameter. They always route through the single global `PlatformConfig::instance`. In practice this is rarely a limitation — all links typically share the same clock and logger.
