# rknpu-submit

A submit provider for [`librocketnpu`](https://github.com/gregordinary/rocket-userspace)
that drives the Rockchip NPU through the vendor `rknpu` BSP kernel driver.

`librocketnpu` drives the mainline `accel/rocket` device. Most Rockchip distributions
ship the BSP kernel instead, where the NPU is an `rknpu` DRM render node or the misc
`/dev/rknpu`. This provider targets that driver's GPL ioctl surface. It accepts the
same CNA→CORE→DPU register programs `librocketnpu` already emits, and so brings those
systems into the open stack. It opens either node flavor, discovering which the kernel
presents.

## Interface

`librocketnpu` routes every kernel interaction through one header, `rocket_npu.h`: device
lifetime, buffer management, cache maintenance, submission, and the capability queries.
This library implements that header against the vendor uAPI. The rest of the stack targets
the silicon, so it compiles unchanged, up through the `ggml`, TFLite and ONNX Runtime
frontends.

The two paths are measured to agree. The same register program produces byte-identical
output through both drivers, checked at M=4 K=32 N=16 and M=64 K=256 N=256 against a host
reference.

Version 0.1.0. This library's interface is `rocket_npu.h`, so the two version together:
the CMake package declares `SameMinorVersion` compatibility, and a consumer that asks for
`0.1` requires `0.1`.

## Requirements

- A Rockchip SoC running a BSP kernel with `rknpu` 0.9.6 or later loaded. Validated on
  RK3588 with 0.9.8, in [Validation](#validation).
- Read and write access to the NPU device node. A DRM render node is conventionally
  `root:render` mode `0660`.
- CMake 3.16 or later and a C11 compiler.
- A `librocketnpu` checkout or installed package.

Grant your account access to the device node once:

```sh
sudo usermod -aG render "$USER"
# log out and back in, then confirm:
id -nG | tr ' ' '\n' | grep -x render
```

Where the node is the misc `/dev/rknpu`, which is commonly root-only, run the steps below
under `sudo -E`. Use `sudo -E` rather than plain `sudo` throughout: the plain form drops the
`ROCKET_*` and `RKNPU_*` settings and `GGML_BACKEND_PATH`.

The vendor uAPI is transcribed in `src/rknpu_uapi.h` with a `_Static_assert` on every
structure size, so a transcription error surfaces as a build failure. Building therefore
needs neither libdrm nor kernel headers, and succeeds on a BSP image that ships no
development headers at all.

## Building

**1. Confirm the driver is loaded and note its version.** The BSP driver versions
independently of the kernel it ships in, so read the version rather than inferring it. The
two node flavors announce themselves differently, and `-i` catches both:

```sh
sudo dmesg | grep -i "Initialized rknpu"
# [drm] Initialized rknpu 0.9.8 20240828 for fdab0000.npu on minor 1  <- DRM_GEM build
# rknpu fdab0000.npu: Initialized RKNPU driver: v0.9.8 for 20240828   <- DMA_HEAP build
```

Device discovery matches the DRM driver name `rknpu`, which is fixed across driver
versions. The version governs the uAPI, and the provider reads it at open and acts on it.
Supported range:

| Driver version | Support |
|---|---|
| 0.9.7 and later | Full. |
| 0.9.6 | Runs. `RKNPU_IOVA_TIGHT` is unavailable, and the provider says so once at open. |
| Below 0.9.6 | Refused at open. |

0.9.6 shipped 2024-03-18, 0.9.7 on 2024-04-24, and 0.9.8 on 2024-08-28, which is the newest
release and the one measured here. What each threshold rests on is in
[Version thresholds](#version-thresholds).

Kernel configuration decides which node the driver presents:
`CONFIG_ROCKCHIP_RKNPU_DRM_GEM`, the default, registers a DRM render node under
`/dev/dri/`, and `CONFIG_ROCKCHIP_RKNPU_DMA_HEAP` registers the misc `/dev/rknpu`. The
provider opens either, finding the render node by DRM driver name because node numbering
varies by board. Sysfs names the driver behind each node:

```sh
ls -l /sys/class/drm/renderD*/device/driver
# .../renderD129/device/driver -> ../../../bus/platform/drivers/RKNPU
```

The platform driver registers as `RKNPU` in capitals (`rknpu_drv.c` in the BSP driver tree),
so sysfs paths take that spelling. The DRM driver name the provider matches is `rknpu`.

**2. Build the provider.** It implements `rocket_npu.h`, so it needs that header alone and
links nothing from `librocketnpu`.

```sh
cmake -S rknpu-submit -B build-rknpu \
      -DROCKETNPU_INCLUDE_DIR=<path>/rocket-userspace/include
cmake --build build-rknpu -j
```

Omit `ROCKETNPU_INCLUDE_DIR` to resolve an installed `rocketnpu` package instead. The
archive is built position-independent, so it links into a shared object, the shape a
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
# 100% tests passed, 0 tests failed out of <n>
```

On an RK3588 the RK3576 gates report `Skipped`, the suite's code for a part that is absent.
The SigLIP and SAM gates skip too, without their model weights. Two gates,
`npu_klog_check` and `uapi_bo_lifetime_rocket`, read the kernel journal. If your account
cannot read it, they skip. `npu_klog_check` fails the run on any NPU job timeout or IOMMU
fault logged during it, so join `adm` to keep it:

```sh
sudo usermod -aG adm "$USER"   # then log out and back in
```

This is the first step that touches the hardware, so it needs the device access from
[Requirements](#requirements). Run the suite serially. The driver shares one IOMMU domain
process-wide, so concurrent jobs compete for the same window. After changing the provider,
rebuild the whole consumer tree. A provider-only rebuild leaves the suite's executables
linked against the previous archive.

### When no device is found

Most often the node is present and this process cannot open it. The provider prints what
each node answered, and that line says which of two cases you have.

**`Permission denied`.** The node exists and refused this process. Join the `render` group
as in [Requirements](#requirements), or re-run under `sudo -E`.

**A driver name other than `rknpu`.** That node belongs to another driver, which is normal
for the display subsystem. Check whether `rknpu` bound at all:

```sh
sudo dmesg | grep -i "Initialized rknpu"  # either "Initialized" line once it probes
ls -d /sys/bus/platform/drivers/RKNPU/*   # the device it bound to, e.g. fdab0000.npu
sudo cat /sys/kernel/debug/rknpu/version
```

If the driver bound and the scan still misses the node, open it directly:

```sh
ROCKET_DEV=/dev/dri/renderD129 sudo -E ./your-binary
```

<details>
<summary>Why two other common checks do not answer this</summary>

`modinfo rknpu` reports the driver the kernel was **built** with. On a BSP kernel it reads
`(builtin)` whichever way the probe went, so it cannot tell you the device is there.

`/sys/class/drm/renderD*/subsystem/version` reports the DRM **core's** version,
`drm 1.1.0 20060810`. It is identical for every node on every machine and names no driver.

The three commands above report the device itself.

</details>

### When an allocation is refused

A gate failure that names an allocation is the shared IOVA window rather than a wrong
answer. The driver maps every buffer into one window shared across the whole process tree.
The suite's largest shapes ask for single buffers of a hundred megabytes and more.

Confirm it:

```sh
sudo dmesg | grep -i iova
```

A reboot is the driver's only reset for the window. The same shape generally passes at
multi-worker fan-out, which splits the buffer into pieces the window can still serve. Keep
`RKNPU_IOVA_TIGHT` at its default of `1`. That is what stops a workload consuming the
window permanently, and [Runtime configuration](#runtime-configuration) has the detail.

<details>
<summary>How fast the window degrades</summary>

The window fragments over a board's uptime, so a large contiguous request can be refused
while smaller ones still fit. A freshly booted board serves about 3.9 GB in buffers of any
size from 16 MB to 256 MB. The same board a day later served 35 buffers of 16 MB, one of
128 MB and none of 192 MB.

`librocketnpu` treats such a refusal as transient where it can, retrying a smaller
allocation before giving up. The usual effect is a slower path rather than a failed one.

</details>

## Running a model: whisper.cpp on the NPU

Steps 1 through 4 leave a `librocketnpu` that reaches the NPU through the vendor driver.
A frontend links that library and inherits the route, so a frontend built the ordinary way
picks the NPU up unmodified. Steps 5 through 8 complete the path for speech recognition,
from a BSP board to stock `whisper.cpp` running its encoder on the NPU. They add one
repository to the two above: [`ggml-rocket`](https://github.com/gregordinary/ggml-rocket),
a `ggml` backend an unmodified host loads through `GGML_BACKEND_PATH`.

**5. Build `whisper.cpp` with a shared, loadable `ggml`.**

```sh
git clone https://github.com/ggml-org/whisper.cpp
cmake -S whisper.cpp -B whisper.cpp/build \
      -DGGML_BACKEND_DL=ON -DBUILD_SHARED_LIBS=ON \
      -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16
cmake --build whisper.cpp/build -j
./whisper.cpp/models/download-ggml-model.sh base.en
```

`GGML_CPU_ARM_ARCH` is required here, because `GGML_BACKEND_DL=ON` forces
`GGML_NATIVE=OFF` and the CPU backend then ships generic armv8 kernels lacking dotprod and
fp16. Such a build runs, at a fraction of the CPU's real speed, and inflates every
NPU-versus-CPU comparison taken afterwards. The run's own `system_info` line is the check:
it reads `DOTPROD = 1 | FP16_VA = 1` when the flag took.

For a quantized model rather than the F16 `.en` models, add `-DGGML_CPU_REPACK=OFF`.
Offload requires the weight in its original layout, so a repacked quantized weight runs on
the CPU and the model completes with the NPU idle.

**6. Build the backend against that host's own bundled `ggml`.**

```sh
git clone https://github.com/gregordinary/ggml-rocket
cmake -S ggml-rocket -B build-ggml-rocket \
      -DGGML_ROCKET_DL=ON \
      -DHOST_DIR=$PWD/whisper.cpp \
      -DGGML_LIB_DIR=$PWD/whisper.cpp/build/bin \
      -DROCKETNPU_DIR=$PWD/rocket-userspace \
      -DROCKETNPU_PROVIDER=external \
      -DROCKETNPU_PROVIDER_LIB=$PWD/build-rknpu/librknpu-submit.a \
      -DROCKETNPU_DRIVER_NAME="vendor rknpu driver"
cmake --build build-ggml-rocket -j
```

`HOST_DIR` and `GGML_LIB_DIR` point at the host application's own bundled `ggml`, and the
backend must be built against it. A `.so` built against a different `ggml` checkout registers no
device. The host's loader refuses a different `GGML_BACKEND_API_VERSION`, and the backend refuses
a host whose op numbering moved.

`ROCKETNPU_DRIVER_NAME` is cosmetic and worth setting. It is what a frontend prints in its
device listing, and it names which kernel path is live. Left unset, an external build
reports `external submit provider`.

`ggml-rocket` builds its own copy of `librocketnpu` from `ROCKETNPU_DIR` and passes the two
provider settings down to it, which is why they are repeated here. The build from step 3 is
what the gate suite in step 4 exercises. An explicit `ROCKETNPU_DIR` takes precedence over
an installed `rocketnpu` package (a `cmake --install`, typically under `/usr/local`), and
the configure log names which of the two it took. Left unset, the installed package wins,
so a machine carrying a stale install builds the backend against a driver nobody named.

**7. Run stock `whisper-cli`.**

```sh
GGML_BACKEND_PATH=$PWD/build-ggml-rocket/libggml-rocket.so ROCKET_KACC=1 \
  ./whisper.cpp/build/bin/whisper-cli \
  -m whisper.cpp/models/ggml-base.en.bin -f audio.wav
```

`ROCKET_KACC=1` is the operating mode to run in. Under `sudo`, use `sudo -E`: the plain
form starts a fresh environment and takes `GGML_BACKEND_PATH` with it, and the run then
loads no backend.

**8. Confirm the NPU actually ran.** Three lines say so. The first two report the device
found and selected:

```
load_backend: loaded ROCKET backend from .../libggml-rocket.so
whisper_backend_init: using ROCKET backend
```

and `ROCKET_MM_PROFILE=1` adds the third, which reports work reaching it:

```
ROCKET profile total(ms): pack=283 (packA=24 packB=18) gen=3 sync=12 submit=497 wait=4 read=57  over 240 job-batches
```

The third line is the one that matters. An op the backend declines runs on the CPU and
still produces a complete, correct, normal-looking transcript. A run that prints the first
two lines and not the third ran its encoder on the CPU.

### Performance

The deliverable is CPU core-seconds handed back to the rest of the system, not wall-clock
speed, and the two are different numbers. The process blocks on the NPU with its host
threads idle, so the CPU freed is larger than the wall saved. `base.en` on a 30 s clip is a
1.10x wall and a 17.4% CPU saving. A wall ratio quoted as if it were a CPU saving is wrong
in both directions depending on the clip.

There are two figures below, because they answer different questions and are not
interchangeable.

**Single-shot**, one clip per process, so the model load sits inside the number. Across
`tiny.en`, `base.en` and `small.en` at 3, 10, 30, 60 and 120 s, the CPU freed is 16-50%.

It rises with model size, and the largest relief is on **short** clips. Every window is
padded to 30 s whatever the audio. So a 3 s utterance pays a full encode and decodes almost
nothing, and the encoder is the part that offloads.

The realtime factor on the NPU arm is seconds of audio per second of wall. It runs 3-33x for
`tiny.en`, 1.8-12.9x for `base.en` and 0.7-5.8x for `small.en`. The sub-realtime cells are
the short clips, where the fixed encode
and the model load dominate.

**Steady state**, the shape a service actually has, with the model loaded once and the cost
taken per utterance on conversational audio. CPU core-seconds per utterance:

| Model | Clip | CPU only | NPU | Freed |
|---|---|---|---|---|
| `tiny.en` | 3 s | 3.03 | 1.93 | 36% |
| `tiny.en` | 10 s | 3.39 | 2.45 | 28% |
| `tiny.en` | 30 s | 4.38 | 3.11 | 29% |
| `base.en` | 3 s | 6.75 | 3.82 | 43% |
| `base.en` | 10 s | 8.37 | 4.89 | 42% |
| `base.en` | 30 s | 9.44 | 6.17 | 35% |

What mostly separates the two figures is the model load, paid once per process: 129 ms for
`tiny.en`, 173 ms for `base.en`, 333 ms for `small.en`. A CLI spawned per transmission pays
it every time and throws much of the saving away, so keep the model loaded.

Measured on an RK3588 (Turing RK1) at 600 MHz on `rknpu 0.9.8` through this provider,
A76-pinned, 4 threads, `ROCKET_KACC=1`. Both arms of every comparison come from one binary,
with the arms interleaved and the warm-up discarded. Discard the first run of anything
measured here, because the clock parks at idle and a cold run reads about 15% low.

### Numerical faithfulness

The NPU encoder is fp16 where the CPU's is fp32, so the two are close rather than
bit-identical. Whether that reaches the transcript depends on the audio. Both arms are
deterministic, and every repetition within an arm is byte-identical, so a difference between
them is the numerics rather than a race.

On clean read speech the transcripts are identical, for all three models at every length
tested. On degraded audio they are not. `base.en` on a two-minute hard conversational clip
diverges by 66 word-level edits, a 153-word NPU transcript against a 169-word CPU one. The
differences run in both directions and neither arm is ground truth. Both are plausible
readings of genuinely ambiguous overlapping speech.

The divergence compounds with length rather than staying local. A perturbation early in an
autoregressive decode changes the prompt context for every window after it.

Radio and telephony audio is degraded audio. A benchmark on clean read speech will show the
two arms agreeing exactly, and will not have tested the case that matters.

### Running as a service

A transcription service keeps `whisper-server` loaded and posts it audio in chunks. OpenWebRX+
(1.2.123 and later) does exactly that. Every `chunkSeconds` of wall time, 20 s by default, it
posts the squelch-gated audio that arrived as one 12 kHz WAV. It sends no other request field
and reads `text` from the reply.

Two things follow. The server's command-line defaults decide every decode parameter, because
the client sends none. And every request costs one full encode,
because whisper pads a window to 30 s whatever the chunk holds.

`whisper-server` is built beside `whisper-cli` in step 5. Start it the way step 7 starts the
CLI, pinned to the A76 cluster:

```sh
GGML_BACKEND_PATH=$PWD/build-ggml-rocket/libggml-rocket.so ROCKET_KACC=1 \
  taskset -c 4-7 ./whisper.cpp/build/bin/whisper-server \
  -m whisper.cpp/models/ggml-small.bin -t 4 -nt -ac 1200 -sns \
  --host 127.0.0.1 --port 8080
```

Four settings decide the cost, and each one is a rule rather than a number:

- **`-ac` is 50 times the chunk length plus 4 s.** That is 1200 for a 20 s chunk, and the
  default of 1500 for a 30 s one. The encoder then covers the audio and the silence after it.
  Set to the chunk length itself, it halves the encode and breaks the decoder. Every chunk
  transcribes correctly to the audio's end and then runs on into a loop. The model never sees
  the silence that closes an utterance.
- **`-nt` (no timestamps) is free.** It returns 12% of the CPU at equal accuracy, and a 30 s
  chunk requires it. With timestamps on, a chunk whose last timestamp lands short of the
  audio's end has its remainder encoded again as a second window. A 30 s chunk is cut mid-word
  at the window's edge, so that happens on most of them. Plain 30 s chunks therefore cost 13%
  more CPU per second of audio than 20 s ones.
- **Send `temperature_inc=0` as a request field** to turn off the temperature fallback. The
  fallback re-decodes a window the decoder is unsure of up to five more times, at about three
  times a normal request's cost. The server's `-nf` flag is parsed and never applied. With
  `-nt` the fallback rarely fires. Turning it off costs a small accuracy margin on the chunks
  it was re-decoding, so it is a judgment call.
- **`-sns` (suppress non-speech tokens) costs nothing** and improves accuracy on every channel
  condition measured. On audio whisper cannot parse, it otherwise emits runs of bracket tokens
  and can loop on them for hundreds of tokens.

Where the client cannot add a field, the server flag is one line away from working, in
`examples/server/server.cpp`:

```diff
-            wparams.temperature_inc  = params.temperature_inc;
+            wparams.temperature_inc  = params.no_fallback ? 0.0f : params.temperature_inc;
```

The table is CPU core-seconds per second of channel time for `ggml-small`, on read speech
through an SSB-shaped channel at 15 dB SNR. It is relative to the server's defaults on 20 s
chunks with the NPU, which are themselves 0.58x the CPU-only run. WER is against the
LibriSpeech reference, on clean speech and on the 15 dB channel, and the chunk cuts set its
floor:

| Configuration | CPU | Request | WER clean / 15 dB |
|---|---:|---:|---|
| CPU only, defaults, 20 s chunks | 1.73 | 7.7 s | 8.9 / 16.8 |
| NPU, defaults, 20 s chunks | 1.00 | 5.3 s | 8.9 / 16.7 |
| NPU, `-nt`, 20 s chunks | 0.96 | 5.0 s | 9.5 / 15.8 |
| NPU, `-nt -ac 1200`, `temperature_inc=0`, 20 s chunks | 0.78 | 4.1 s | 9.3 / 15.2 |
| NPU, `-nt -ac 1200 -sns`, `temperature_inc=0`, 20 s chunks | 0.77 | 4.1 s | 7.7 / 13.8 |
| NPU, `-nt`, `temperature_inc=0`, 25 s chunks | 0.85 | 5.0 s | 8.3 / 15.0 |
| NPU, `-nt`, `temperature_inc=0`, 30 s chunks | 0.74 | 5.8 s | 5.3 / 19.0 |
| NPU, defaults, 30 s chunks | 1.21 | 9.6 s | 5.8 / 14.0 |

The 20 s rows keep the latency and return 22-23% of the CPU at the same or better accuracy.
The 30 s row returns 26% with the best clean-speech accuracy and a two- to five-point loss on the
noisy channel, at 30 s of latency. A second speaker confirmed the ranking. Across all three channel conditions the `-sns` row reads 0.70 and
the 30 s row 0.65.

Two things do not help. A quantized model costs more here. `ggml-small-q8_0` takes 10% more CPU
than F16, because whisper leaves its weight tensors unnamed. That keeps the quantized encoder
off the resident route, so every request dequantizes it again. And `-t 2` saves 12% of the CPU
for a request half again as long, with the same transcript. That is a trade rather than a
saving.

Measured on an RK3588 (Turing RK1) at 600 MHz, A76-pinned, 4 threads, `ROCKET_KACC=1`, on
whisper.cpp master at `eacbd82`. The server is billed by its own CPU time per request, and the
arms are rotated across two passes. These are host-side whisper.cpp settings, so they carry
across the driver. The provider numbers above are the vendor-path ones.

## Other frontends

Two other frontends reach this NPU by the same route: each links `librocketnpu`, which
steps 1 through 4 have already pointed at the vendor driver. Those steps are unchanged in
both cases, and only the frontend build differs.

### transcribe.cpp: further model families

[`transcribe.cpp`](https://github.com/handy-computer/transcribe.cpp) (MIT) is a `ggml`-based
multi-model speech-to-text library: Whisper, Parakeet, Canary, Granite Speech, Voxtral,
SenseVoice and more. `GGML_BACKEND_PATH` is a `ggml` facility, so it picks the backend up
the way `whisper.cpp` does. Steps 5 and 6 become:

```sh
git clone https://github.com/handy-computer/transcribe.cpp
cmake -S transcribe.cpp -B transcribe.cpp/build \
      -DTRANSCRIBE_BUILD_SHARED=ON -DTRANSCRIBE_GGML_BACKEND_DL=ON \
      -DTRANSCRIBE_VULKAN=OFF -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16
cmake --build transcribe.cpp/build -j

cmake -S ggml-rocket -B build-gr-transcribe \
      -DGGML_ROCKET_DL=ON \
      -DHOST_DIR=$PWD/transcribe.cpp \
      -DGGML_LIB_DIR=$PWD/transcribe.cpp/build/ggml/src \
      -DROCKETNPU_DIR=$PWD/rocket-userspace \
      -DROCKETNPU_PROVIDER=external \
      -DROCKETNPU_PROVIDER_LIB=$PWD/build-rknpu/librknpu-submit.a \
      -DROCKETNPU_DRIVER_NAME="vendor rknpu driver"
cmake --build build-gr-transcribe -j
```

`GGML_CPU_ARM_ARCH` is load-bearing for the same reason as in step 5. `GGML_LIB_DIR` is this
host's `build/ggml/src` rather than its `build/bin`, and the CPU module is found separately.

**Two gaps in the host's own dynamic-backend path need working around**, neither of them
this provider's and neither reported as what it is:

- The CLI registers dynamic backends only on its `--list-devices` path. A shared build then
  loads no backend at all when it transcribes, and stops with `whisper: failed to initialize
  CPU backend`. Calling `transcribe_init_backends_default()` once before the model load in
  `examples/cli/main.cpp` is the whole fix.
- The backend scan reads only the directory the library itself sits in. The Arm CPU module
  is a plain `libggml-cpu.so` elsewhere in the build, so run `cp build/bin/libggml-cpu.so
  build/src/`. Without it only the NPU registers, and the run fails as above.

Then run it, with the module search path pointed at the build and the NPU supplied the same
way:

```sh
cd transcribe.cpp
LD_LIBRARY_PATH=$PWD/build/ggml/src:$PWD/build/bin:$PWD/build/src \
GGML_BACKEND_PATH=../build-gr-transcribe/libggml-rocket.so ROCKET_KACC=1 \
  ./build/bin/transcribe-cli -m <model>.gguf audio.wav
```

Its proof line reads `whisper: using accel backend: ROCKET`, and `ROCKET_MM_PROFILE=1`
adds the same profile line, which remains the only evidence that work reached the device.
`--list-devices` shows `kind=accel`, which is what puts the NPU ahead of the CPU in its
scheduler.

Measured on the validation board with `whisper-base.en-Q8_0` on an 11 s clip, medians of
three interleaved runs:

| Arm | Wall | CPU core-seconds |
|---|---:|---:|
| NPU | 1.88 s | 9.00 |
| CPU | 2.97 s | 21.70 |

That is 1.58x wall and 58.5% of the core-seconds handed back, with the transcripts
identical. Both arms take this CLI's default of every core, little cores included. Its
core-second figures are therefore roughly twice `whisper-cli`'s at four threads, and the
ratio is not comparable to the tables above. Pinning to the big cores is worth more than it
is here.

### tflite-rocket: object detection

[`tflite-rocket`](https://github.com/gregordinary/tflite-rocket) is a TensorFlow Lite external
delegate: a `.so` that any TFLite host loads at runtime and runs against an unmodified
`.tflite` model. It links `librocketnpu` directly, so steps 5 onward collapse to one build:

```sh
git clone https://github.com/gregordinary/tflite-rocket
cmake -S tflite-rocket -B build-tflite-rocket \
      -DTFLITE_DIR=/path/to/tflite-c-headers \
      -DROCKETNPU_DIR=$PWD/rocket-userspace \
      -DROCKETNPU_PROVIDER=external \
      -DROCKETNPU_PROVIDER_LIB=$PWD/build-rknpu/librknpu-submit.a
cmake --build build-tflite-rocket -j
```

`TFLITE_DIR` is a header root. The delegate is a classic C `TfLiteDelegate` and needs only
`tensorflow/lite/core/c/{common,builtin_op_data}.h` plus `tensorflow/lite/builtin_ops.h`.
The build also produces `libtflite_cshim.so`, which supplies the two TFLite C-API symbols
the delegate binds at `dlopen`. The classic `tflite_runtime` and full `tensorflow` wheels
export them. LiteRT (`ai_edge_litert`, the wheel on recent Python) keeps them internal, so
under LiteRT, `LD_PRELOAD` the shim. Without it the delegate stops at
`undefined symbol: TfLiteIntArrayCreate`.

```sh
LD_PRELOAD=$PWD/build-tflite-rocket/libtflite_cshim.so python3 - <<'EOF'
from ai_edge_litert.interpreter import Interpreter, load_delegate, OpResolverType
d = load_delegate("build-tflite-rocket/libtflite_rocket.so", options={"native_int8": "1"})
it = Interpreter(model_path="ssdlite_mobiledet_coco_qat_postprocess.tflite",
                 experimental_delegates=[d],
                 experimental_op_resolver_type=OpResolverType.BUILTIN_WITHOUT_DEFAULT_DELEGATES)
it.allocate_tensors(); it.invoke()
EOF
```

`OpResolverType.BUILTIN_WITHOUT_DEFAULT_DELEGATES` is required: it holds XNNPACK back so
the convolutions are offered to the external delegate first. Under the default resolver
XNNPACK claims them and the model runs entirely on the CPU while its log still reads as
delegated. The delegate's `profile=1` option prints the per-op line that settles it.

Both boards were clock-matched at 600 MHz, and both builds came from the same sources.
Measured that way against the mainline `rocket` driver, **the detectors' outputs are
identical at the byte**.

An MD5 over every output tensor of SSDLite-MobileDet and EfficientDet-Lite0 agrees across the
two drivers. That holds in the CPU, `native_int8=1` and `native_int8=0` arms, with every
on-NPU auxiliary route enabled, and on the fp16-NCHW resident path. Warm single-inference latency is 202.1 ms against
mainline's 197.9 for MobileDet and 299.6 against 293.6 for EfficientDet-Lite0. That 2%
falls on a host-bound workload, and the two boards' CPU ceilings differ by as much on their
own. Four concurrent detection processes, the shape a
multi-camera deployment takes, produce the single-process output hash on both drivers and
aggregate 3.51x against mainline's 3.61x.

## Runtime configuration

| Variable | Default | Effect |
|---|---|---|
| `ROCKET_DEV` | probe by driver name | Open a specific node, for a multi-NPU box or a test rig. |
| `RKNPU_CORE_MASK` | `0` (driver schedules) | Pin submits to one core: `1`, `2` or `4`. Single-core values only; the driver commits the same task range to every core named. |
| `ROCKET_BATCH_SUBMIT` | `1` (chained) | `0` issues one submit per program in the gapped layout. |
| `RKNPU_IOVA_TIGHT` | `1` | `0` selects the kernel's generic IOVA mapping path, which permanently consumes the shared IOMMU domain. Requires `rknpu` 0.9.7; below that the provider withholds the flag and says so at open. |
| `RKNPU_BO_SENTINEL` | `0` | Fill fresh buffers with a given byte, so an all-zero readback separates "the device wrote zeros" from "the device never wrote". |

`RKNPU_IOVA_TIGHT` protects the IOMMU domain, and is worth understanding before turning it
off. This driver maps every buffer through **one domain shared across the whole process**.

On the generic path a single 2048-token prefill (Llama-3.2-3B F16, 153 s) permanently costs
that domain 5-11 of the 31 128 MiB buffers it can serve. The loss outlives the process, so
only a reboot resets it. With the flag set the same run costs **zero**. Throughput is
identical either way (39.90-41.39 tokens/s at 2048, 67.57 at 512 in both arms), so the flag
buys headroom rather than speed.

Two consequences follow:

- **Alongside a stock RKNN userspace**, which takes the generic path, this process can meet
  `-ENOMEM` while the domain still holds space it cannot reach. A large refusal is worth
  retrying smaller, and `librocketnpu` does that where it can.
- **A board with unknown uptime can already be degraded.** Check the domain before trusting
  an allocation-sensitive measurement on one.

The kernel-side mechanism, and the measurements that isolated it, are in the comment above
`rknpu_iova_tight_flag()` in `src/rknpu_provider.c`.

## Driver characteristics

Reference for readers tuning the defaults above or changing the provider. The vendor uAPI
differs from the mainline driver in buffer identity, job structure and submit semantics. The
provider absorbs each difference, and the table states the behavior and where it surfaces.

| Behavior | This driver | How the provider expresses it |
|---|---|---|
| Buffer identity | `MEM_DESTROY` and `MEM_SYNC` key on the memory object's address | `rocket_bo` carries an `obj_addr` field |
| Task array | Read through the object's *kernel* mapping | Allocated with `RKNPU_MEM_KERNEL_MAPPING`; one growable buffer per open file |
| Submit | Blocks until the job retires (`RKNPU_JOB_BLOCK` is 0; `NONBLOCK` is the flag) | The caller's wait-on-output is already satisfied on return |
| Multi-program job | One contiguous register-command stream whose trailers link, kicked once | `librocketnpu`'s chained layout, on by default |
| Job scope | One job per submit; the core is released between jobs | CBUF operand reuse is gated on a chained layout |
| Completion | The job's last task supplies `INT_MASK` | The terminal block is named per program |

### Version thresholds

**Below 0.9.6, `rocket_open()` refuses and names the version it found.** The memory
allocation interface changed in 0.9.6, and this provider speaks only the later form, so
every allocation would fail. Refusing at open makes that one clear message instead of an
unexplained failure partway into a run.

**On 0.9.6, `RKNPU_IOVA_TIGHT` is unavailable.** The flag it sets arrived in 0.9.7, and an
older driver accepts it and ignores it. The provider therefore withholds it and says so at
open.
Buffers then take the generic path, with the consequences under
[Runtime configuration](#runtime-configuration).

**0.9.7 and 0.9.8 are one target.** The 0.9.8 release left the user-facing interface
untouched, so 0.9.7 is supported by construction rather than by a run, and is untested on
hardware [expected].

The structure-level differences behind these thresholds are in `src/rknpu_uapi.h`.

### Chained submission and operand reuse

The driver programs `PC_DATA_ADDR` and `PC_DATA_AMOUNT` from the first task, writes the
task count into `PC_TASK_CONTROL`, and kicks once. A job of *n* tasks is therefore one
contiguous stream of *n* register programs whose own trailers link to the next. That is the
layout `librocketnpu`'s `rocket_chain.c` builds, and the multi-program shape this uAPI takes
natively. It needs no kernel patch here.

Because each submit is its own job, the driver takes the next entry off its per-core
`todo_list` as soon as one retires. Another thread's or another process's job can land
between two programs.

CBUF operand reuse depends on the previous task's operand still being resident, so it is
valid exactly when the batch is chained. With reuse enabled and an unchained batch, one
128×1024×1024 fp16 matmul at three-way fan-out produced a corrupt result on 29 of 120 runs.
That is against 0 of 120 with reuse disabled and 0 of 80 chained. The corruption is a full,
plausible output surface that moves between runs.

`librocketnpu` queries this directly. `rocket_submit_batch_atomic()` reports whether a
batch runs as one job (0 here, 1 on mainline) and gates reuse on the answer.
`rocket_batched_submit_native()` reports that the chained layout is native to this uAPI, and
`ROCKET_BATCH_SUBMIT` takes its default from that.

### Naming the completing block

The driver completes a job when the interrupt status collapses to the `INT_MASK` its last
task carries. Each program therefore names the block it finishes on. A program ending in a
DPU write uses the DPU pair (`0x300`). A pooling program ends on the PPU pair (`0xc00`),
which the batched-job `PPU_DONE` flag selects.

Naming a block that fires earlier retires the job at that point. Hardware elapsed time falls
from 121 to 79 microseconds with a `0x3` mask at 64×256×256. The mask therefore follows the
program's terminal stage.

## Throughput

512×3840×4096 fp16 through the resident multicore path, best of five repetitions after
three warm rounds, RK3588 at the vendor driver's 1000 MHz idle clock:

| Configuration | Best (ms) | GOP/s |
|---|---|---|
| Core 0 pinned, unchained | 68-77 | 208-236 |
| Core 0 pinned, chained | 36.8-39.3 | 409-437 |
| Driver core scheduler, unchained | 47.5-48.3 | 333-339 |
| **Driver core scheduler, chained (the defaults)** | **26.1-27.7** | **581-617** |
| The same shape through mainline `rocket`, for reference | 23.5-24.4 | 660-685 |

The two defaults compose. Letting the driver schedule cores gives the library's multi-core
fan-out somewhere to fan out to. The chained layout removes both the extra ioctls and the
reuse restriction.

## Instruments

`include/rknpu_submit.h` exposes the measurement surface this uAPI carries:

- `rknpu_last_hw_elapse_ns()`: hardware elapsed time of the last submit on a file
  descriptor. The driver stamps it as a `ktime` pair around the register-program commit, so
  it spans the register write burst, execution and interrupt latency. It is scoped to the
  most recent submit on that descriptor.
- `rknpu_action()`: the raw action ioctl behind the clock, voltage, IOMMU-domain, SRAM
  and DMA-counter queries.

Query availability before planning a measurement. The DMA byte counters, the SRAM and NBUF
pools, and bandwidth QoS are wired on RK3576, RK356x, RK3562, RV1106 and RV1126B.

On RK3588 the driver configuration leaves the counter registers unset and the NBUF size at
zero. Those queries return 0, and the driver logs `Get rw_amount is not supported on this
device!`. Per-submit hardware elapsed time, the clock and voltage queries, and the IOMMU
domain identifier are available there.

## Validation

Measured on an RK3588 (Turing RK1), Armbian `6.1.172-vendor-rk35xx`, `rknpu 0.9.8
20240828`, NPU at `/dev/dri/renderD129`, on 2026-09-26.

The path was walked from fresh clones, following the steps above as written:

| Component | Version walked | Result |
|---|---|---|
| `rknpu-submit` | `db7c3ab` | Configures and builds with no errors, and `provider_seam` passes |
| `rocket-userspace` | `c7e7c47` | Builds. The full suite passes: 76 of 97 run, and 21 skip as step 4 describes |
| `ggml-rocket` | `cfcc0f5` | Builds against the provider, and `libggml-rocket.so` loads |
| `whisper.cpp` | master `d09f61a` | `whisper-cli` prints all three lines of step 8, and its `samples/jfk.wav` transcript matches the CPU run's |
| `transcribe.cpp` | `c6a9257` | Selects the NPU as an `accel` device and offloads, with the two host-side workarounds its section names |
| `tflite-rocket` | unrecorded | `convert_test` at 220 cases and 0 failures; six driver-level probes pass, including a 1494-shape CBUF bank-slack sweep at zero error; detector outputs byte-identical to mainline |

The `transcribe.cpp` and `tflite-rocket` rows are from an earlier walk on
`6.1.115-vendor-rk35xx`, with the same driver.

`ctest` in `build-rknpu`, the provider's own build directory, runs `provider_seam`. It
needs no hardware: it checks that this provider defines every symbol `rocket_npu.h`
requires, which a compiler cannot check on its own. It registers only against a
`librocketnpu` source checkout, since that is where the symbol list lives.

## License

GPL-3.0-or-later.
