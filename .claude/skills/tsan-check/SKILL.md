---
name: tsan-check
description: Build and run the test suite under ThreadSanitizer to prove EncounterEngine locking and the multi-producer IPC ring buffer are intact. Use after touching any EncounterEngine, CombatantRegistry or MetricsAccumulator entry point, when changing what locks the combat meter takes, or when touching src/common/ipc/ring_buffer.hpp or adding a thread that pushes outbound packets - a race there is invisible to a normal `make test` and only reports under TSan.
---

# Checking `EncounterEngine` Locking

`MeterEngine.ConcurrentProducersAndReaders` drives the engine from four threads at once,
mirroring the in-game topology. A missing lock does not fail an ordinary run - the test
passes and the race stays silent. It only reports under ThreadSanitizer:

```bash
clang++ -std=c++20 -fsanitize=thread -g -O1 \
  -Iinclude -Isrc -Itests -Iplugins -Iplugins/latency_mitigator/include -Iplugins/combat_meter/include \
  src/common/*.cpp src/common/ipc/*.cpp src/common/config/*.cpp src/common/os/*.cpp src/common/ui/*.cpp \
  plugins/*/src/*.cpp src/payload/*.cpp src/app/app_state.cpp src/app/ui/*.cpp tests/*.cpp \
  -o tsan_runner && ./tsan_runner
```

Run it after touching any `EncounterEngine`, `CombatantRegistry` or `MetricsAccumulator`
entry point, `src/common/ipc/ring_buffer.hpp`, or the set of threads that push outbound
packets. It must report zero data races.

## What it is protecting

In-game, four threads reach one engine: the `ReceiveActionEffect`/`ProcessHotDot` detours
on the game's main thread, the payload orchestration thread (`sync_party`, `set_zone`,
`update`, and the vitals pass: status lists, life events, `tracked_enemies`), the DX11 `Present` thread rendering `CombatOverlay`, and the `PipeClient`
reader thread dispatching commands.

Every public entry point takes `EncounterEngine::m_mutex`, which is recursive because the
lifecycle calls re-enter each other. The registry must be reached through
`with_registry()`; the raw `registry()`/`accumulator()` accessors do not lock and are for
single-threaded use only. Adding an entry point that skips the mutex, or reaching the
registry through a raw accessor from one of those four threads, is exactly what this run
catches.

## The outbound packet queue

The same run covers `IPC.PacketRingBufferConcurrentProducers`. It pushes onto the real
`PacketRingBuffer` from two threads, the way the game's detour thread and the payload
orchestration thread do, while one consumer drains it. `PacketRingBuffer` gives each
producer thread its own SPSC lane (root `AGENTS.md`, invariant 3). If the alias is
pointed back at a plain `SpscRingBuffer`, or a lane ends up with two producers, TSan
reports the race on the slot's `std::vector`. The test also fails on its own when a
packet is torn, lost or duplicated.
