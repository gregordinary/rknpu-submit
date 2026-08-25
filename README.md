# rknpu-submit

A submit provider for [`librocketnpu`](https://github.com/gregordinary/rocket-userspace)
that drives the Rockchip NPU through the vendor `rknpu` BSP kernel driver.

Most Rockchip distributions ship the BSP kernel, where the NPU appears as an `rknpu` DRM
render node or as the misc `/dev/rknpu` node rather than as a mainline `accel/rocket`
device. This provider targets that driver's GPL ioctl surface, which accepts the same
CNA→CORE→DPU register programs `librocketnpu` already emits, and makes the NPU reachable
on those systems from the open stack. Both node flavours are handled: they are compiled
from one handler and differ only in the ioctl magic, and the provider discovers which is
present at open.

## How it fits

`librocketnpu` isolates every kernel interaction behind one header, `rocket_npu.h`: device
open and close, buffer allocation and teardown, cache maintenance, submit, and the
capability and counter queries. This library defines those symbols against the vendor
uAPI. Everything above the seam — the register encoders, the tiling, the on-NPU op
library, the graph planner, and the `ggml`, TFLite and ONNX Runtime frontends — is shaped
by the silicon rather than by the driver, and compiles unchanged.

The two paths are measured to agree. The same register program produces byte-identical
output through both drivers: FNV-1a `0a003896b051d7cb` at M=4 K=32 N=16 and
`cacd57a95d6d0441` at M=64 K=256 N=256, each matching the host reference.

Version 0.1.0. What this library exports is `librocketnpu`'s submit seam, so its interface
moves with that library's; the CMake package declares `SameMinorVersion` compatibility, and
a consumer that asks for `0.1` is not satisfied by `0.2`.

## Requirements

- A Rockchip SoC running a BSP kernel with the `rknpu` driver loaded. Validated on
  RK3588; see [Validation](#validation).
- CMake 3.16 or later and a C11 compiler.
- A `librocketnpu` checkout or installed package.

The vendor uAPI is transcribed in `src/rknpu_uapi.h` with a `_Static_assert` on every
structure size, so a transcription error surfaces as a build failure. Building therefore
needs neither libdrm nor kernel headers, and succeeds on a BSP image that ships no
development headers at all.

## Building

**1. Confirm the driver is loaded and note its version.** The BSP driver versions
independently of the kernel it ships in, and the field decode below is gated on it.

```sh
sudo dmesg | grep "Initialized rknpu"
# [drm] Initialized rknpu 0.9.8 20240828
```

The provider finds the device itself, by asking each `/dev/dri/renderD12x` node for its
DRM driver name and falling back to `/dev/rknpu`. Node numbering follows probe order and
varies by board, so a name is the only reliable identifier.

**2. Build the provider.** It defines the seam's symbols rather than calling them, so only
`librocketnpu`'s header is needed here and the archive does not link it.

```sh
cmake -S rknpu-submit -B build-rknpu \
      -DROCKETNPU_INCLUDE_DIR=<path>/rocket-userspace/include
cmake --build build-rknpu -j
```

Omit `ROCKETNPU_INCLUDE_DIR` to resolve an installed `rocketnpu` package instead. The
archive is built position-independent, so it links into a shared object — the shape a
frontend drop-in takes.

**3. Build `librocketnpu` against it.** The consumer links the core library and the
provider together.

```sh
cmake -S rocket-userspace -B build \
      -DROCKETNPU_PROVIDER=external \
      -DROCKETNPU_PROVIDER_LIB=$PWD/build-rknpu/librknpu-submit.a
cmake --build build -j
```

**4. Verify against the hardware.** The test suite doubles as the bring-up gate and runs
on the device.

```sh
cd build && ctest --output-on-failure -j1 --timeout 300
# 93 tests passed out of 93
```

Run the suite serially: the driver shares one IOMMU domain process-wide, so concurrent
jobs compete for the same window. Where the device node needs privilege, use `sudo -E`
rather than `sudo` so the `ROCKET_*` and `RKNPU_*` settings survive into the test
environment.

After changing the provider, rebuild the whole consumer tree. A provider-only rebuild
leaves the suite's executables linked against the previous archive.

A failure that names an allocation rather than a miscompare is the IOVA window and not a
wrong answer. The driver maps every buffer into one window shared across the whole process
tree, and the suite's largest shapes ask for single buffers of a hundred megabytes and more;
when the window cannot serve one the call refuses, and the gate reports the refusal. The
same shape generally passes at multi-worker fan-out, which splits the buffer. `dmesg | grep
IOVA` separates this from a genuine miscompare, and `RKNPU_IOVA_TIGHT` under
[Runtime configuration](#runtime-configuration) is the margin.

That window fragments over a board's uptime, so a large contiguous request can be refused while
smaller ones still fit. A freshly booted board serves about 3.9 GB in buffers of any size from
16 MB to 256 MB; the same board a day later served 35 buffers of 16 MB, one of 128 MB and none
of 192 MB. Rebooting restores it, and the driver offers no lighter reset. `librocketnpu` treats
such a refusal as transient where it can, retrying a smaller allocation before giving up, so the
usual effect is a slower path rather than a failed one.

## Running a model: whisper.cpp on the NPU

The steps above leave a `librocketnpu` that reaches the NPU through the vendor driver.
Nothing in a frontend knows about this provider — the seam is below them — so a frontend
built the ordinary way picks the NPU up unmodified. This is the whole path for speech
recognition, from a BSP board to stock `whisper.cpp` running its encoder on the NPU. It
adds one repository to the two above:
[`ggml-rocket`](https://github.com/gregordinary/ggml-rocket), a `ggml` backend an
unmodified host loads through `GGML_BACKEND_PATH`.

**5. Build `whisper.cpp` with a shared, loadable `ggml`.**

```sh
git clone https://github.com/ggml-org/whisper.cpp
cmake -S whisper.cpp -B whisper.cpp/build \
      -DGGML_BACKEND_DL=ON -DBUILD_SHARED_LIBS=ON \
      -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16
cmake --build whisper.cpp/build -j
./whisper.cpp/models/download-ggml-model.sh base.en
```

`GGML_CPU_ARM_ARCH` is not optional here. `GGML_BACKEND_DL=ON` forces `GGML_NATIVE=OFF`,
and the CPU backend then ships generic armv8 kernels with neither dotprod nor fp16. Nothing
fails; the CPU simply runs slower than the machine can, which inflates every NPU-versus-CPU
comparison taken afterwards. The run's own `system_info` line is the check — it must read
`DOTPROD = 1 | FP16_VA = 1`.

If the model is a quantized one rather than the F16 `.en` models, add `-DGGML_CPU_REPACK=OFF`.
A repacked quantized weight cannot be offloaded, so repacking removes the NPU from the run
silently rather than failing.

**6. Build the backend against that host's own bundled `ggml`.**

```sh
git clone https://github.com/gregordinary/ggml-rocket
cmake -S ggml-rocket -B build-ggml-rocket \
      -DGGML_ROCKET_DL=ON \
      -DHOST_DIR=$PWD/whisper.cpp \
      -DGGML_LIB_DIR=$PWD/whisper.cpp/build/bin \
      -DROCKETNPU_DIR=$PWD/rocket-userspace \
      -DROCKETNPU_PROVIDER=external \
      -DROCKETNPU_PROVIDER_LIB=$PWD/build-rknpu/librknpu-submit.a
cmake --build build-ggml-rocket -j
```

Against the host application's bundled `ggml`, not a separate `ggml` checkout: the backend
vtable is positional, so a mismatched `GGML_BACKEND_API_VERSION` yields a `.so` that loads
and whose device never appears — a silent absence rather than an error.

`ggml-rocket` builds its own copy of `librocketnpu` from `ROCKETNPU_DIR` and passes the two
provider settings down to it, which is why they are repeated here. The build from step 3 is
not wasted: it is the one the gate suite in step 4 exercises. If a `rocketnpu` package is
already installed — a `cmake --install`, typically under `/usr/local` — that package is used
and `ROCKETNPU_DIR` is ignored. The configure log distinguishes the two cases, and
`-DCMAKE_DISABLE_FIND_PACKAGE_rocketnpu=ON` forces the source tree.

**7. Run stock `whisper-cli`.**

```sh
GGML_BACKEND_PATH=$PWD/build-ggml-rocket/libggml-rocket.so ROCKET_KACC=1 \
  ./whisper.cpp/build/bin/whisper-cli \
  -m whisper.cpp/models/ggml-base.en.bin -f audio.wav
```

`ROCKET_KACC=1` is the operating mode to run in. Where the device node needs privilege, use
`sudo -E` rather than `sudo`, which strips the environment and takes `GGML_BACKEND_PATH`
with it.

**8. Confirm the NPU actually ran.** A registered device and a used device are different
things: an op the backend declines runs on the CPU, and the result is a complete, correct,
normal-looking transcript. Two lines say the device was found and selected,

```
load_backend: loaded ROCKET backend from .../libggml-rocket.so
whisper_backend_init: using ROCKET backend
```

and `ROCKET_MM_PROFILE=1` adds the line that says work actually reached it:

```
ROCKET profile total(ms): pack=283 (packA=24 packB=18) gen=3 sync=12 submit=497 wait=4 read=57  over 240 job-batches
```

A run that prints no such line offloaded nothing, however healthy the rest of the log looks.

### What this is worth

The deliverable is CPU core-seconds handed back to the rest of the system, not wall-clock
speed, and the two are different numbers. The process blocks on the NPU with its host
threads idle, so the CPU freed is larger than the wall saved: `base.en` on a 30 s clip is a
1.10x wall and a 17.4% CPU saving. A wall ratio quoted as if it were a CPU saving is wrong
in both directions depending on the clip.

There are two figures below, because they answer different questions and are not
interchangeable.

**Single-shot**, one clip per process, so the model load sits inside the number. Across
`tiny.en`, `base.en` and `small.en` at 3, 10, 30, 60 and 120 s, the CPU freed is 16-50%. It
rises with model size, and the largest relief is on **short** clips: every window is padded
to 30 s whatever the audio, so a 3 s utterance pays a full encode and decodes almost
nothing, and the encoder is the part that offloads. The realtime factor on the NPU arm —
seconds of audio per second of wall — is 3-33x for `tiny.en`, 1.8-12.9x for `base.en` and
0.7-5.8x for `small.en`; the sub-realtime cells are the short clips, where the fixed encode
and the model load dominate.

**Steady state**, the shape a service actually has, with the model loaded once and the cost
taken per utterance on conversational audio. `base.en` goes from 6.75 to 3.82 CPU
core-seconds on a 3 s utterance (43%), 8.37 to 4.89 at 10 s (42%) and 9.44 to 6.17 at 30 s
(35%). `tiny.en` goes from 3.03 to 1.93 (36%), 3.39 to 2.45 (28%) and 4.38 to 3.11 (29%).

What mostly separates the two is the model load — 129 / 173 / 333 ms for `tiny.en` / `base.en` /
`small.en`, paid once per process. A CLI spawned per transmission pays it every time and
throws much of the saving away; keep the model loaded.

Measured on an RK3588 (Turing RK1) at 600 MHz on `rknpu 0.9.8` through this provider,
A76-pinned, 4 threads, `ROCKET_KACC=1`, both arms of every comparison from one binary with
the arms interleaved and the warm-up discarded. Discard the first run of anything measured
here: the clock parks at idle, so a cold run reads about 15% low.

### Numerical faithfulness

The NPU encoder is fp16 where the CPU's is fp32, so the two are close and not bit-identical,
and whether that reaches the transcript depends on the audio. Both arms are deterministic —
every repetition within an arm is byte-identical — so a difference between them is the
numerics and not a race.

On clean read speech the transcripts are identical, for all three models at every length
tested. On degraded audio they are not: `base.en` on a two-minute hard conversational clip
diverges by 66 word-level edits, a 153-word NPU transcript against a 169-word CPU one. The
differences run in both directions and neither arm is ground truth — both are plausible
readings of genuinely ambiguous overlapping speech. The divergence compounds with length
rather than staying local, because a perturbation early in an autoregressive decode changes
the prompt context for every window after it.

Radio and telephony audio is degraded audio. A benchmark on clean read speech will show the
two arms agreeing exactly, and will not have tested the case that matters.

## Runtime configuration

| Variable | Default | Effect |
|---|---|---|
| `ROCKET_DEV` | probe by driver name | Open a specific node, for a multi-NPU box or a test rig. |
| `RKNPU_CORE_MASK` | `0` (driver schedules) | Pin submits to one core: `1`, `2` or `4`. Single-core values only — the driver commits the same task range to every core named. |
| `ROCKET_BATCH_SUBMIT` | `1` (chained) | `0` issues one submit per program in the gapped layout. |
| `RKNPU_IOVA_TIGHT` | `1` | `0` selects the kernel's generic IOVA mapping path, which rounds each mapping up to the next power of two. |
| `RKNPU_BO_SENTINEL` | `0` | Fill fresh buffers with a given byte, so an all-zero readback separates "the device wrote zeros" from "the device never wrote". |

`RKNPU_IOVA_TIGHT` selects `RKNPU_MEM_IOMMU_LIMIT_IOVA_ALIGNMENT`, which maps each buffer
at its true size. On the rounding path a working set exhausts the IOVA window and the
library absorbs the failed allocations onto the CPU: roughly 108 per repetition of a
2048-token prefill against about 3 with tight mapping. Throughput at 512 tokens is
unchanged either way — 67.57 tokens/s in both arms over four interleaved rounds with the
board to itself — so the flag buys headroom rather than speed, and the window remains the
binding constraint on a larger model. Buffers allocated on this path are prefilled and
flushed from the CPU before first use, which establishes the mapping for the device's
writes.

## Driver characteristics

The vendor uAPI expresses several things differently from the mainline driver. The
provider absorbs each of them; the table states the behavior and where it surfaces.

| Behavior | This driver | How the provider expresses it |
|---|---|---|
| Buffer identity | `MEM_DESTROY` and `MEM_SYNC` key on the memory object's address | `rocket_bo` carries an `obj_addr` field |
| Task array | Read through the object's *kernel* mapping | Allocated with `RKNPU_MEM_KERNEL_MAPPING`; one growable buffer per open file |
| Submit | Blocks until the job retires (`RKNPU_JOB_BLOCK` is 0; `NONBLOCK` is the flag) | The caller's wait-on-output is already satisfied on return |
| Multi-program job | One contiguous register-command stream whose trailers link, kicked once | `librocketnpu`'s chained layout, on by default |
| Job scope | One job per submit; the core is released between jobs | CBUF operand reuse is gated on a chained layout |
| Completion | The job's last task supplies `INT_MASK` | The terminal block is named per program |

### Chained submission and operand reuse

The driver programs `PC_DATA_ADDR` and `PC_DATA_AMOUNT` from the first task, writes the
task count into `PC_TASK_CONTROL`, and kicks once. A job of *n* tasks is therefore one
contiguous stream of *n* register programs whose own trailers link to the next — the
layout `librocketnpu`'s `rocket_chain.c` builds, and the multi-program shape this uAPI
takes natively. It needs no kernel patch here.

Because each submit is its own job, the driver takes the next entry off its per-core
`todo_list` as soon as one retires, and another thread's or another process's job can land
between two programs. CBUF operand reuse depends on the previous task's operand still
being resident, so it is valid exactly when the batch is chained: with reuse enabled and
an unchained batch, one 128×1024×1024 fp16 matmul at three-way fan-out produced a corrupt
result on 29 of 120 runs, against 0 of 120 with reuse disabled and 0 of 80 chained. The
corruption is a full, plausible output surface that moves between runs.

`librocketnpu` queries this directly. `rocket_submit_batch_atomic()` reports whether a
batch runs as one job (0 here, 1 on mainline) and gates reuse on the answer;
`rocket_batched_submit_native()` reports that the chained layout is native to this uAPI,
and `ROCKET_BATCH_SUBMIT` takes its default from that.

### Naming the completing block

The driver completes a job when the interrupt status collapses to the `INT_MASK` its last
task carries, so each program names the block it finishes on. A program ending in a DPU
write uses the DPU pair (`0x300`); a pooling program ends on the PPU pair (`0xc00`), which
the batched-job `PPU_DONE` flag selects. Naming a block that fires earlier retires the job
at that point — measured as hardware elapsed time falling from 121 to 79 microseconds with
a `0x3` mask at 64×256×256 — so the mask follows the program's terminal stage.

## Throughput

512×3840×4096 fp16 through the resident multicore path, best of five repetitions after
three warm rounds, RK3588 at the vendor driver's 1000 MHz idle clock:

| Configuration | Best (ms) | GOP/s |
|---|---|---|
| Core 0 pinned, unchained | 68–77 | 208–236 |
| Core 0 pinned, chained | 36.8–39.3 | 409–437 |
| Driver core scheduler, unchained | 47.5–48.3 | 333–339 |
| **Driver core scheduler, chained (the defaults)** | **26.1–27.7** | **581–617** |
| The same shape through mainline `rocket`, for reference | 23.5–24.4 | 660–685 |

The two defaults compose. Letting the driver schedule cores gives the library's multi-core
fan-out somewhere to fan out to, and the chained layout removes both the extra ioctls and
the reuse restriction.

## Instruments

`include/rknpu_submit.h` exposes the measurement surface this uAPI carries:

- `rknpu_last_hw_elapse_ns()` — hardware elapsed time of the last submit on a file
  descriptor. The driver stamps it as a `ktime` pair around the register-program commit,
  so it spans the register write burst, execution and interrupt latency, and is scoped to
  the most recent submit on that descriptor.
- `rknpu_action()` — the raw action ioctl behind the clock, voltage, IOMMU-domain, SRAM
  and DMA-counter queries.

Query availability before planning a measurement. The DMA byte counters, the SRAM and NBUF
pools, and bandwidth QoS are wired on RK3576, RK356x, RK3562, RV1106 and RV1126B. On
RK3588 the driver configuration leaves the counter registers unset and the NBUF size at
zero, so those queries return 0 and the driver logs `Get rw_amount is not supported on
this device!`; per-submit hardware elapsed time, the clock and voltage queries, and the
IOMMU domain identifier are available there.

## Validation

Validated on an RK3588 (Turing RK1, Armbian `6.1.115-vendor-rk35xx`, `rknpu 0.9.8
20240828`) against the NPU at `/dev/dri/renderD129`. On that board `renderD128` is the
display subsystem, which is why the node is found by DRM driver name.

Read `GET_DRV_VERSION` and gate on it before relying on the field decode described above:
the driver versions independently of the kernel it ships in.

`ctest` in the *provider's* build directory — `build-rknpu` above, not the driver's `build`,
whose suite is the hardware gate of step 4 — runs one gate, `provider_seam`, which needs no
hardware: it asks the driver tree what the submit seam is and checks this provider defines all
of it. A seam that grows breaks nothing here at compile time — a provider defines those
symbols rather than calling them — so without the gate the first sign is an undefined
reference in whoever links the two together. The gate registers only against a
`librocketnpu` source checkout, since the seam is read from a file an installed package does
not ship.

The end-to-end path was walked on that board at these commits, from fresh clones of the three
repositories and following the steps above as written, with `whisper.cpp` at master
(`v1.9.3-75-g9781133`): the provider, `librocketnpu` and `libggml-rocket.so` each built with no
errors, and stock `whisper-cli` printing all three lines of step 8 — the backend loaded, the
backend selected, and the profile line that says work reached the device.

## Licence

GPL-3.0-or-later.
