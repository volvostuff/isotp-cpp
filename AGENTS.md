# AGENTS.md

## What this is

C++ wrapper library for the [isotp-c](https://github.com/SimonCahill/isotp-c)
ISO-TP (ISO 15765-2) C library. One `Link` represents one ISO-TP connection:
messages are sent on `txId` (also used for the flow-control frames we emit while
receiving), frames from the peer arrive on `rxId`.

Event driven: call `send()` to start a transmission, feed received CAN frames to
`onCanFrame()`, and call `poll()` periodically so multi-frame sends are paced
and timeouts are detected. Whole received messages / transmission completion
arrive through the callbacks passed to the constructor. Timing runs on an
injectable 32-bit microsecond clock (`setClock`) - tests use a fake clock.

## Structure

- `isotp-c/` — **git submodule** pointing to upstream C library (pinned). Do not
  edit files here; changes belong in the upstream repo.
- `include/isotp_cpp/isotp.hpp` — public API (`namespace isotp_cpp`): `Result`,
  `CanDriver`, `Link`, `setClock`/`setLogger`.
- `src/isotp.cpp` — wrapper plus the `isotp_user_*` C hooks the upstream library
  requires.
- `tests/` — dependency-free test runner over an in-memory fake bus and clock.
- `CMakeLists.txt` — builds `isotp_cpp`; tests via `ISOTP_CPP_BUILD_TESTS`.

## Build & test (Windows/MSVC; same flow works with GCC/ESP-IDF)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\tests\Release\isotp_cpp_tests.exe     :: exit 0 = pass
```

Use `Release`: the upstream library emits `/O2` outside single-config Debug
builds, which conflicts with MSVC Debug `/RTC1`.

## Feature macros (public ABI)

The wrapper relies on upstream options that change the `IsoTpLink` layout and
add callbacks, so these are declared `PUBLIC` on the `isotp` target (in addition
to upstream's private defines):

- `ISO_TP_TRANSMIT_COMPLETE_CALLBACK`
- `ISO_TP_RECEIVE_COMPLETE_CALLBACK`
- `ISO_TP_USER_SEND_CAN_ARG` — per-link `void*` argument to
  `isotp_user_send_can`, used to route frames to the owning `Link`.

Configuration: classic CAN only (`ISO_TP_MAX_CAN_FRAME_SIZE = 8`), payload up to
`isotp_cpp::kMaxPayload` (4095). BS/STmin and the response timeout use the
upstream compile-time defaults.

## Notes

- License: BSD 3-Clause (see `LICENSE`). The vendored `isotp-c` is MIT and keeps
  its own LICENSE inside the submodule; do not reformat either here.
- Threading: one `Link` is intended to be driven from a single thread. Multiple
  links may coexist; the clock/logger are module-global.
