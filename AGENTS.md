# AGENTS.md

## What this is

C++ wrapper library for the [isotp-c](https://github.com/SimonCahill/isotp-c) ISO-TP (ISO 15765-2) C library. Currently a skeleton — no wrapper code exists yet.

## Structure

- `isotp-c/` — **git submodule** pointing to upstream C library. Do not edit files here directly; changes belong in the upstream repo.
- Root-level `.gitignore` covers C/C++ build artifacts and CMake output.

## Submodule details (isotp-c/)

The submodule has its own build system, independent of this repo:

- **CMake**: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release`
- **Make**: `make` (Linux/macOS, uses `vars.mk`)
- **Tests**: Google Test via CMake with `isotpc_ENABLE_TESTING=ON`. Tests live in `isotp-c/tests/`.
- Key CMake options: `isotpc_MAX_CAN_FRAME_SIZE`, `isotpc_PAD_CAN_FRAMES`, `isotpc_ENABLE_STREAMING`, `isotpc_ENABLE_CAN_FD_BRS`, `isotpc_ENABLE_CAN_SEND_FLAGS`.

## Notes

- No root-level build system, linter, formatter, or CI config exists yet.
- No C++ wrapper headers or source files exist yet — this is the work to be done.
- License: BSD 3-Clause.
