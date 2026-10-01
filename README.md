# Angry Birds 1.0 for the iPod nano 7G

An engine reimplementation that runs the original game's Lua bytecode and converted
assets. The Mac harness is playable; the nano app builds and has been installed on
the existing NanoApps test device. The user confirmed that repacking asset pages to
256×128 fixed the reboot entering level 1. A later test rebooted during a pig impact
in level 1-2. A fix for repeated scenery rebuilding and oversized generated textures
was confirmed much smoother by the user; its device report averaged 23.7 FPS.
The latest build also replaces full-heap allocation searches with free lists;
its latest 30-second device sample averaged 45.9 FPS with zero allocation failures.
The subsequent renderer update reduces draw calls by 36–38% in fixed-input host
replays. Its first 30-second device sample averaged 46.9 FPS, with draw submission
averaging 2.49 ms versus 3.49 ms before; the playthroughs used different inputs.
The next build adds faster bulk memory operations and more detailed cadence
measurement; its device result is pending.
The FPS counter and recorder remain enabled.
Audio is currently silent on both platforms.

No game binary or assets are tracked here. The converter expects an existing
Angry Birds 1.0 `AngryBirds.app/data` directory.

## Build and test on macOS

The converter requires Pillow, NumPy and texture2ddecoder (installed in the project's
local `.venv`). OpenGL access is required for the host harness, including headless tests.

```sh
.venv/bin/python tools/convert_assets.py v1.0/Payload/AngryBirds.app/data port/build/data-small-pages --max-page 256 --max-page-height 128
make -C port -j4
python3 tools/smoke_host.py
cd port
./build/abhost -w -d build/data-small-pages
```

The texture scale is 0.5. The installed test profile caps asset pages at 256×128,
matching the page bounds used by the working Mario Kart renderer. Large sprites
are reduced further to fit; small sprites keep their original requested scale.
Display coordinates remain 576×320, rendered onto 432×240 pixels. Larger asset
pages remain available for host comparisons. Runtime ground-mask atlases now use
up to eight 256×128 pages, with only one 64 KiB pixel buffer during construction.
The nano renderer rejects textures exceeding the device page limits.

The smoke test clears level 1-1, checks a 31,320 score, verifies save reload, and
repeats with the portrait framebuffer rotation used on the nano. Tests use temporary
save directories and do not overwrite normal saves. It also continues into level
1-2 (internally `Level57`), throws three birds, verifies pig-impact scoring, and
checks texture memory and cache rebuild counts with ±30% frame-time jitter.

## Build the nano app

Use the existing NanoApps SDK checkout and a Linux ARM bare-metal toolchain with
newlib. This app targets the SDK's firmware 1.1.2 / 39A10023 environment; other
firmware builds have not been validated.

```sh
make -C port/nano NANOAPPS=/path/to/NanoApps -j4
mkdir -p /path/to/NanoApps/apps/angrybirds
cp port/nano/app/* /path/to/NanoApps/apps/angrybirds/
make -C /path/to/NanoApps/apps/angrybirds AB_DIR=/absolute/path/to/port -j4
```

The engine build rejects static constructors; the application build checks Thumb
code for unsupported floating-point instructions and validates relocations.
The profiling app is about 257 KiB on disk and uses a separate allocation arena plus up to
3 MiB of texture pixel data. Driver overhead is additional.

## Prepare an installation

`tools/stage_nano.py` builds a file-only stage from an existing app list. It preserves
other apps' bundle bytes, adds Angry Birds, validates asset file sizes, and writes
checksums. It does not access a device.

```sh
.venv/bin/python tools/stage_nano.py \
  --sdk /path/to/NanoApps \
  --app /path/to/angrybirds.hbapp \
  --data port/build/data-small-pages \
  --base-pack /path/to/current/AllApps-B.pack \
  --out port/build/new-stage
```

The stage contains `/Apps/Executables/Angry Birds.hbapp`, its icon,
`/Apps/Data/AngryBirds/`, and the augmented `/Apps/AllApps-B.pack`.
Copy data and executable first, verify readbacks, and replace the app list last.
Existing high scores and settings are not part of the stage. The current test
installation's previous app list is retained in `port/build/AllApps-B.before.pack`
and on the mini PC under `~/nano-ab/device-backup-20261001/`.

## Verified so far

- Nano compile, link, static-constructor check, Thumb-FP check and `.hbapp` packaging.
- Host level 1-1 completed and save reloaded in both screen orientations.
- With the small-page profile, level 1-1 peaks at 2,072 KiB of texture data.
  The previous larger-page profile also loaded displayed level 12 on the host;
  that does not establish hardware compatibility.
- All 59 repacked asset pages and 826 sprite rectangles validated.
- The compiled nano engine completes level 1 in QEMU using its actual allocator,
  math routines, Lua and Box2D, including a run with the minimum 2 MiB arena.
  Graphics are mocked; this cannot verify the GPU or its allocation overhead.
  This run scores 31,270, versus 31,320 in the Mac build, so numerical parity
  remains incomplete.
- The level 1-2 collision replay passes 4,600 ARM frames with frame-time jitter
  and a 2 MiB arena: pig-impact score 17,090, five ground atlas builds, 2,458 KiB
  peak texture data including the FPS HUD. No allocation failures were reported.
- Rendering now records complete frames so texture uploads precede draw calls,
  following the device-driver workaround already used by the Mario Kart port.
- Device installation: 123 files copied and read back successfully. Existing Mario
  64 and Mario Kart app entries preserved; no firmware or loader code changed.
- Subsequent small-page update: 156 data files (including the manifest), the app,
  and a one-launch trace flag copied and read back. The previous executable and
  complete data directory are backed up on the mini PC at
  `~/nano-ab/device-backup-20261001/before-small-pages/`.

On hardware, hold the nano with Home on the right. Home exits the app; relaunching
starts it fresh. Long-session stability still needs hardware confirmation.
The engine analysis notes are under `analysis/`;
they are evidence for the reimplementation, not proof of complete behavioral parity.

The ARM regression wrapper can be run on the Linux build host after building the
nano library:

```sh
cd port
python3 tests/qemu/build.py /path/to/NanoApps
qemu-arm -cpu cortex-a7 build/qemu/abtest
# Model 8 MiB free before the game allocates its minimum 2 MiB arena:
python3 tests/qemu/build.py /path/to/NanoApps 8
qemu-arm -cpu cortex-a7 build/qemu/abtest
```

It also rejects texture or vertex-buffer uploads after the first draw of a frame.

For a hardware diagnostic run, create `/Apps/Data/AngryBirds/trace.once` before
launching. The app consumes the flag and saves at most 96 load/upload/draw checkpoints
to `log.txt`, including OS free memory and the arena's largest free block. Reconnect
after a failure without relaunching the app, so startup does not overwrite the log.
Normal launches do not write these checkpoints.

## Level 1-2 cache fix and device measurements

The pre-fix device report measured 179 callbacks in 30.073 seconds: about 5.9 FPS,
with 152.7 ms average callback work. Physics averaged 0.3 ms; game work averaged
119.8 ms, including 19.5 ms of file IO and 4.6 ms of texture uploads. The report's
tail was zero-filled from byte 512, so its missing fields are not usable evidence.

A host reproduction identified a cache bug: interpolation can change a stationary
object's render coordinates by a floating-point rounding unit. The renderer treated
each change as movement and rebuilt the ground atlas. By frame 2,800, uneven timing
caused 566 atlas builds and 2,902 reads, versus one build and 77 reads with steady
timing. The cache now uses the static body's actual pose. The longer 4,600-frame
host regression, including three shots, needs five builds and 97 reads. Its peak
texture data is 2,456 KiB. These are work counts, not device FPS predictions.

Generated ground textures now follow the small-page limits already used for assets;
the old 512×512 runtime upload is gone. This addresses a known driver risk, but a
real-device level 1-2 replay is still needed to confirm the reported reboot is fixed.

The subsequent device report contains 712 callbacks in 30.025 seconds, averaging
23.7 FPS (42.17 ms between callbacks; p95 64 ms). Callback work averaged 27.03 ms:
23.48 ms game update, 3.37 ms draw submission, 0.86 ms physics, 0.27 ms IO and
0.04 ms texture upload. It recorded three mask rebuilds. The user confirmed much
smoother play; extended stability remains unverified.

To run the matching ARM regression after building the library:

```sh
cd port
python3 tests/qemu/build.py /path/to/NanoApps 8 build/data-small-pages tests/level2.txt 4600 jitter
qemu-arm -cpu cortex-a7 build/qemu/abtest
```

The profiling build displays FPS in the lower-right corner. This uses the interval
between device frame callbacks, including time spent outside the game callback;
it is not the reciprocal of CPU work time. After a two-second level warmup it
saves an initial report after five measured seconds, then replaces it once after
30 measured seconds. Samples survive level changes; each new level adds its own
two-second warmup. At most two writes occur per launch, at the start of the next
callback before new GL work. The interval containing the recorder's own disk write
is excluded from the measurements. The compact v5 report fits within 512 bytes and includes mean
FPS, frame-time percentiles, slow-frame counts, game/physics/draw/IO/upload timings.
`fps10` is FPS multiplied by ten; times are microseconds except `p95_ms`, whose
histogram is capped at 200 ms. Physics, IO and upload times are subsets of game
time. `mask_builds` counts scenery rebuilds during the measured window; the
draw, quad and texture fields describe its final frame.
`draw_avg`, `merge_avg`, and `state_avg` average draw calls, merged batches, and
batch-state GL calls across the measured window. State calls exclude frame setup,
texture uploads, and vertex/color pointer setup.
In v5, `gap` averages the directly timed gap from the end of the previous platform
callback to the next frame's start (including unmeasured entry work), while `vbo`
times the vertex-buffer upload and is a subset of submission time. `min` is the
shortest callback interval; `at60` counts intervals at or below 16,667 microseconds.
Neither field establishes that every frame meets 60 FPS.
The HUD adds one small texture and two draw batches. Do not enable `trace.once`
for a performance measurement, because those diagnostic writes add stalls.

The previous recorder required 30 seconds within one level and discarded its
samples on every transition, leaving an older report after short playthroughs.
The v3 tests cover early/final reports, short-level transitions, uneven frame
timing, and exclusion of the recorder's own write delay.

## Allocator optimization

The original allocator searched from the start of the entire arena for every
allocation. During the ARM replay, ten small allocations in one frame visited
151,674 blocks. The new allocator searches size-binned free lists, merges adjacent
free blocks immediately, and resizes allocations in place when possible. It keeps
the existing 2–4 MiB arena policy and the OS/graphics memory reserve.

In the same 4,600-frame ARM replay with a 2 MiB arena:

| Work | Before | After |
| --- | ---: | ---: |
| Allocation searches | 34,141 | 34,068 |
| Blocks examined | 291,029,743 | 505,422 |
| Bytes copied by realloc | 10,680 | 8,336 |
| Final level 1-2 score | 17,880 | 17,880 |

The new run reports zero allocation failures. The 99.8% reduction measures allocator
search work. On-device reports also include average
allocation/search/copy counts per frame and total allocation failures for the window.

The subsequent v3 hardware report recorded 1,378 callbacks in 30.001 seconds:

| Device measurement | Before allocator change | After |
| --- | ---: | ---: |
| Average FPS | 23.7 | 45.9 |
| Mean callback interval | 42.17 ms | 21.77 ms |
| 95th-percentile interval | 64 ms | 35 ms |
| Frames exceeding 50 ms | 199/712 (28.0%) | 7/1,378 (0.5%) |
| Mean app callback work | 27.03 ms | 6.81 ms |
| Mean game update | 23.48 ms | 3.13 ms |

These are separate user playthroughs, so the scenes and inputs are not controlled.
The latest report records zero allocation failures, 210 allocator probes per frame,
and 2,042 KiB of texture data in its final frame. The worst interval was 140.85 ms;
occasional stalls remain. About 14.97 ms of the mean interval occurs outside the
measured app callback, which includes scheduling/presentation overhead but does
not identify a specific cause. This sample does not establish long-session stability.

The allocator stress test checks alignment, data preservation, failed growth,
shrinking under memory pressure, free-list consistency and full coalescing after
100,000 randomized operations. Run it with sanitizers on the host:

```sh
cc -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
  port/tests/allocator_test.c -o port/build/allocator_test
port/build/allocator_test
```

## Renderer optimization

Sprite corners are now transformed once for both culling and drawing. GL texture,
clipping, and client-array state is reused within a frame, with state caches reset
on every callback because the resident also uses the GL context.

Compatible batches can be combined across intervening draws only when their bounds
are disjoint, including a one-physical-pixel margin. Overlapping draw order is
preserved. Search is limited to eight earlier batches, and vertex copying is capped
at 4,096 vertices per frame. This adds about 4 KiB of batch bounds; it adds no texture
pages or heap allocation and retains upload-before-draw ordering and batch limits.

Fixed-seed Mac replays, identical in both display orientations:

| Replay | Draws before merging | Draws after | Reduction |
| --- | ---: | ---: | ---: |
| Level 1-1, 2,300 frames | 46,240 | 29,701 | 35.8% |
| Level 1-2, 4,600 uneven frames | 111,679 | 69,369 | 37.9% |

All 496 sampled screenshots match the previous renderer pixel-for-pixel. The
OpenGL fixture in `port/tests/renderer_test.c` also matches the previous renderer
for transparency, overlapping/disjoint draws, clipping, blend changes, merged
vertex offsets, texture retirement, and resident state changes between callbacks;
both builds pass AddressSanitizer and UndefinedBehaviorSanitizer. The host smoke
test checks level completion, scoring, save reload, and level 1-2 impacts.

The ARM replay with the minimum 2 MiB arena completes 4,600 frames, scoring 17,880
with five mask builds and zero allocation failures. Including the FPS HUD, it
submits 75,121 draws and combines 45,483 batches. These are workload counts with
mocked graphics, not a device benchmark.

The first v4 device report after the renderer update recorded 1,410 callbacks in
30.021 seconds:

| Device measurement | Allocator build (v3) | Renderer build (v4) |
| --- | ---: | ---: |
| Reported average FPS | 45.9 | 46.9 |
| Mean callback interval | 21.77 ms | 21.29 ms |
| 95th-percentile interval | 35 ms | 35 ms |
| Frames exceeding 50 ms | 7/1,378 (0.51%) | 3/1,410 (0.21%) |
| Worst interval | 140.85 ms | 67.22 ms |
| Mean app callback work | 6.81 ms | 6.63 ms |
| Mean draw submission | 3.49 ms | 2.49 ms |

Draw submission averaged 28.6% less time. Mean game work increased from 3.13 ms
to 3.95 ms, including physics increasing from 0.37 ms to 0.90 ms, so these samples
represent different workloads. The v4 report averages 21 draws, 12 merged batches,
and 33 batch-state calls per frame, with three mask builds and zero allocation
failures. Its final frame uses 1,978 KiB of texture data. About 14.66 ms of the
average callback interval remains outside measured app work; the report cannot
separate scheduling, presentation, and other resident activity. The modest FPS
increase and reduced stalls in this sample do not establish a repeatable gain or
long-session stability.

To compare two host builds that contain the `AB_REPLAY_SEED` test hook:

```sh
.venv/bin/python tools/compare_render.py /path/to/reference/abhost
```

The hook only controls the host's Lua random seed; it is excluded from nano builds.

## Bulk memory optimization and the 60 FPS target

Inspection of the linked app found that its SDK `memcpy` copied one byte per loop
and newlib's overlapping backward `memmove` also used byte loops. Draw batching
uses both routines. App-specific linker wrappers now copy aligned words in groups
of sixteen bytes, including backward overlap, and accelerate `memset` too. They
retain byte handling for incompatible alignments and tails, never access beyond
the requested range, and use integer instructions so they work before FP setup.
The SDK itself is unchanged. The FPS HUD now formats its text only when the
displayed value changes, rather than every frame.

The wrappers pass 276,776 alignment, overlap, bounds, and data-preservation cases
under host AddressSanitizer/UndefinedBehaviorSanitizer and in the ARM regression.
The 4,600-frame ARM replay retains score 17,880, five mask builds, the same render
counts, and zero allocation failures. The extra vertex-upload timing preserves
all 496 reference screenshots. The app's Thumb-FP and relocation checks pass.
The update adds roughly 1 KiB to the executable and 60 bytes of static storage.

```sh
cc -std=c99 -g -O2 -fno-builtin -fsanitize=address,undefined \
  -fno-omit-frame-pointer port/tests/memory_test.c port/nano/nano_memory.c \
  -o port/build/memory_test
port/build/memory_test
cc -std=c99 -O2 port/tests/perf_test.c port/nano/nano_perf.c -o port/build/perf_test
port/build/perf_test sixty
```

The new build has not yet been measured on hardware. The latest measured result
remains 46.9 FPS. The GL surface app interface exposes a frame callback, without
an app-selectable callback interval. The directly measured gap in v5 will help
distinguish time between callbacks from the game work we can optimize; no 60 FPS
claim is made from host timing or the simulated ARM cadence.
