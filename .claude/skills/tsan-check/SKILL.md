---
name: tsan-check
description: Build and run the test suite under ThreadSanitizer to prove EncounterEngine locking and the multi-producer IPC ring buffer are intact. Use after touching any EncounterEngine, CombatantRegistry or MetricsAccumulator entry point, when changing what locks the combat meter takes, or when touching src/common/include/common/ipc/ring_buffer.hpp or adding a thread that pushes outbound packets - a race there is invisible to a normal `make test` and only reports under TSan.
---

# Checking `EncounterEngine` Locking

`MeterEngine.ConcurrentProducersAndReaders` drives the engine from more threads at once
than the game does, covering the in-game topology. A missing lock does not fail an ordinary run - the test
passes and the race stays silent. It only reports under ThreadSanitizer:

```bash
make tsan
```

This builds the same sources as `make` with `-fsanitize=thread -g -O1` into
`build/tsan/` (clang++ unless `CXX` says otherwise) and runs the suite with
`halt_on_error=1`, so the first race fails the target. CI runs it on every pull request
(the *Linux ThreadSanitizer* job), but run it locally after touching any
`EncounterEngine`, `CombatantRegistry` or `MetricsAccumulator` entry point,
`src/common/include/common/ipc/ring_buffer.hpp`, or the set of threads that push
outbound packets. It must report zero data races.

Clang needs its sanitizer runtime (`libclang-rt-<version>-dev` on Debian and Ubuntu); a
link error naming `libclang_rt.tsan` means it is missing. On a kernel with 32-bit mmap
ASLR entropy, the LLVM 18 runtime aborts with "unexpected memory mapping" before any test
runs; `sudo sysctl -w vm.mmap_rnd_bits=28` works around it, as CI does.

## What it is protecting

In-game, three threads reach one engine: the `ReceiveActionEffect`/`ProcessHotDot`
detours on the game's main thread, the payload orchestration thread (`sync_party`,
`set_zone`, `update`, the vitals pass: status lists, life events, `tracked_enemies`, enemy
HP, and every command from the app), and the DX11 `Present` thread rendering
`CombatOverlay`. The `PipeClient` reader thread only pushes commands into `CommandQueue`;
the orchestration loop drains and dispatches them.

Every public entry point takes `EncounterEngine::m_mutex`, which is recursive because the
lifecycle calls re-enter each other. The registry must be reached through
`with_registry()`; the raw `registry_unlocked()`/`accumulator_unlocked()` accessors do not
lock and are for tests only. Adding an entry point that skips the mutex, or reaching the
registry through a raw accessor from one of those threads, is exactly what this run
catches.

## The command queue and overlay geometry

`Payload.CommandQueueCarriesReaderCommandsToTheLoopInOrder` pushes commands from one
thread, as the pipe reader does, while another drains and dispatches them onto a real
overlay. `Payload.OverlayGeometryIsAConsistentSnapshotAcrossThreads` writes an overlay's
geometry from one thread, as `render()` does every frame, while another reads it, as the
geometry push and the autosave do. Dispatching from the reader again, or dropping the
lock around `OverlayBase`'s geometry, reports here.

## The outbound packet queue

The same run covers `IPC.PacketRingBufferConcurrentProducers`. It pushes onto the real
`PacketRingBuffer` from two threads, the way the game's detour thread and the payload
orchestration thread do, while one consumer drains it. `PacketRingBuffer` gives each
producer thread its own SPSC lane (root `AGENTS.md`, invariant 3). If the alias is
pointed back at a plain `SpscRingBuffer`, or a lane ends up with two producers, TSan
reports the race on the slot's `std::vector`. The test also fails on its own when a
packet is torn, lost or duplicated.
