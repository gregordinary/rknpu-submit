# rknpu-submit

## AI disclosure

Except for the prior work it builds on, rknpu-submit was developed by AI, primarily Claude. Human
involvement was mostly limited to setting project goals and providing hardware access. This is a
side project for curiosity's sake, and it comes with no guarantee of quality, accuracy, or update
frequency.

## About rknpu-submit

rknpu-submit is a submit provider for [`librocketnpu`](https://github.com/gregordinary/rocket-userspace)
that drives the Rockchip NPU through the `rknpu` driver, the BSP kernel driver most Rockchip
images ship.

`librocketnpu` drives the mainline `accel/rocket` device. Most Rockchip distributions ship the
BSP kernel instead, where the NPU is an `rknpu` DRM render node. This provider implements
`librocketnpu`'s [submit seam](#terms), `rocket_npu.h`, against the `rknpu` driver's GPL ioctl
interface. The CNA→CORE→DPU register programs `librocketnpu` emits then run on a BSP kernel
unchanged. The frontends built on it, the `ggml`, TFLite and ONNX Runtime ones, run unchanged too.

The two paths agree. The same register program produces byte-identical output through both
drivers, checked against a host reference at matmul shapes (M×K×N) 4×32×16 and 64×256×256
[HW sweep].

The provider is version 0.1.0. Its CMake package declares `SameMinorVersion` compatibility, so a
consumer that asks for `0.1` gets a 0.1.x release.

## Driver support

| `rknpu` driver | Support |
|---|---|
| 0.9.7 and later, DRM render node | Supported. |
| 0.9.6, DRM render node | Supported, without `RKNPU_IOVA_TIGHT`. The provider says so once per process. |
| Below 0.9.6 | Not supported. `rocket_open()` refuses and names the version it found. |
| The misc `/dev/rknpu` node, any version | Not supported. `rocket_open()` refuses and names the build. |

Validated on the RK3588 with 0.9.8 (see [Validation](#validation)). The driver's releases are
0.9.6 on 2024-03-18, 0.9.7 on 2024-04-24 and 0.9.8 on 2024-08-28. 0.9.8 was the newest release
when this was validated.

The driver versions independently of the kernel it ships in, and the provider reads the version
at open:

- **Below 0.9.6.** The memory allocation interface changed in 0.9.6, and this provider speaks
  only the later form, so every allocation would fail. The provider refuses at open with one
  message instead.
- **0.9.6.** The flag `RKNPU_IOVA_TIGHT` sets arrived in 0.9.7, and an older driver accepts it
  and ignores it. The provider withholds it, and buffers take the generic mapping path described
  under [`RKNPU_IOVA_TIGHT`](#rknpu_iova_tight).
- **0.9.7 and 0.9.8.** The 0.9.8 release left the user-facing interface unchanged, so 0.9.7 is
  supported by construction. It is untested on hardware [expected].

The structure-level differences behind these thresholds are in `src/rknpu_uapi.h`.

The kernel configuration decides which node the driver presents.
`CONFIG_ROCKCHIP_RKNPU_DRM_GEM`, the default, registers a DRM render node under `/dev/dri/`. The
provider finds it by its DRM driver name, `rknpu`, which is fixed across driver versions, because
node numbering varies by board. `CONFIG_ROCKCHIP_RKNPU_DMA_HEAP` registers the misc `/dev/rknpu`
instead. That build rejects `RKNPU_MEM_MAP` and cannot map a buffer into the process, so the
provider cannot allocate on it [source-confirmed: BSP `rknpu_drv.c`, 0.9.8]. `rocket_open()`
refuses the node with one message.

## Terms

The terms this README uses that the kernel's and the frontends' documentation does not define:

| Term | Definition | Source | Nearest standard term |
|---|---|---|---|
| CBUF | Convolution buffer. The on-chip buffer of an NPU core that holds the feature data and weights the core computes on. | NVDLA | |
| Chained batch | Several register programs submitted as one job, each program's trailer linking to the next, so the NPU runs them from one kick. | Coined | Command-buffer chaining |
| IOVA window | The range of I/O virtual addresses (IOVA) through which the NPU reaches its buffers. Every buffer an NPU job uses takes a range in it. | Coined | IOMMU address space |
| Operand reuse | A task reading an operand that the previous task left in the CBUF, instead of loading it again from memory. | Coined | Data reuse |
| Provider | A library that implements the submit seam for one kernel driver. `librocketnpu` builds the mainline `rocket` provider in, and this project is the `rknpu` one. | Coined | Driver backend |
| Register program | The sequence of register writes that one NPU task executes, from the CNA through the CORE to the DPU. | Coined | Command buffer |
| Submit seam | The header, `rocket_npu.h`, through which `librocketnpu` makes every kernel call: device lifetime, buffer management, cache maintenance, submission and the capability queries. | Coined | Hardware abstraction layer |

## Requirements

- A Rockchip SoC running a BSP kernel with the `rknpu` driver 0.9.6 or later loaded. Validated
  on the RK3588 with 0.9.8.
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

Where a step runs under `sudo`, use `sudo -E`: the plain form drops the `ROCKET_*` and `RKNPU_*`
settings and `GGML_BACKEND_PATH`.

The build needs neither libdrm nor kernel headers, so it succeeds on a BSP image that ships no
development headers. `src/rknpu_uapi.h` contains the provider's own copy of the `rknpu` uAPI.
A `_Static_assert` on the size of each ioctl argument structure makes a copying error a build
failure.

## Build and verify

### 1. Driver check

The driver versions independently of the kernel it ships in, so read its version rather than
inferring it. The two node flavors announce themselves differently, and `-i` matches both:

```sh
sudo journalctl -b -k | grep -i "Initialized rknpu"
# ... kernel: [drm] Initialized rknpu 0.9.8 20240828 for fdab0000.npu on minor 1  <- DRM_GEM build
# ... kernel: RKNPU fdab0000.npu: RKNPU: Initialized RKNPU driver: v0.9.8 for 20240828  <- DMA_HEAP build
```

The second form is the misc-node build, which the provider does not support (see
[Driver support](#driver-support)).

The line prints once, when the driver probes, which is normally at boot. The journal keeps this
boot's kernel messages. `dmesg` reads a fixed-size ring buffer that rolls over. On a board that
has been up for days, it can return nothing for a driver that did probe. Where there is no
journal, run `sudo dmesg | grep -i "Initialized rknpu"` instead, and read an empty result as
unproven.

Sysfs names the driver behind each render node:

```sh
ls -l /sys/class/drm/renderD*/device/driver
# .../renderD129/device/driver -> ../../../bus/platform/drivers/RKNPU
```

The platform driver registers as `RKNPU` in capitals (`rknpu_drv.c` in the BSP driver tree), so
sysfs paths take that spelling. The DRM driver name the provider matches is `rknpu`.

### 2. Provider build

The provider compiles against `librocketnpu`'s headers, `rocket_npu.h` and
`rocket_hw_profile.h`. Step 3 links the two together:

```sh
cmake -S rknpu-submit -B build-rknpu \
      -DROCKETNPU_INCLUDE_DIR=<path>/rocket-userspace/include
cmake --build build-rknpu -j
```

Omit `ROCKETNPU_INCLUDE_DIR` to resolve an installed `rocketnpu` package instead. The archive is
built position-independent, so it links into a shared object, the form a frontend drop-in takes.

### 3. `librocketnpu` build

The consumer links the core library and the provider together:

```sh
cmake -S rocket-userspace -B build \
      -DROCKETNPU_PROVIDER=external \
      -DROCKETNPU_PROVIDER_LIB=$PWD/build-rknpu/librknpu-submit.a
cmake --build build -j
```

### 4. Hardware test

The test suite is the bring-up check, and it runs on the device:

```sh
cd build && ctest --output-on-failure -j1 --timeout 300
# 100% tests passed, 0 tests failed out of <n>
```

Step 4 is the first step that opens the device, so it needs the device access from
[Requirements](#requirements). Run the suite serially: every buffer it allocates takes a range
in the one IOVA window the driver maps, and concurrent tests compete for it.

On an RK3588 the RK3576 tests report `Skipped`, the suite's code for a part that is absent. The
SigLIP, SAM and ModernBERT tests skip too, without their model weights. Two tests,
`npu_klog_check` and `uapi_bo_lifetime_rocket`, read the kernel journal. If your account cannot
read it, they skip. `npu_klog_check` fails the run on a job timeout or an IOMMU fault logged during it, so
join `adm` to keep it:

```sh
sudo usermod -aG adm "$USER"   # then log out and back in
```

`npu_klog_check` matches the `rknpu` driver's `job timeout`, `job abort` and
`failed to wait job` lines as well as the mainline driver's message.

After changing the provider, rebuild the whole consumer tree. A provider-only rebuild leaves the
suite's executables linked against the previous archive.

## Troubleshooting

### No device found

Most often the node is present and this process cannot open it. The provider prints what each
node answered, and that line says which case you have.

**`Permission denied`.** The node exists and refused this process. Join the `render` group as in
[Requirements](#requirements), or run under `sudo -E`.

**`is the rknpu misc node`.** No render node is `rknpu` and `/dev/rknpu` is present, so the
kernel was built with the misc-node driver, which the provider does not support (see
[Driver support](#driver-support)). `rocket_open()` prints this instead of the scan.

**A driver name other than `rknpu`.** That node belongs to another driver, which is normal for
the display subsystem. Check whether `rknpu` bound at all:

```sh
sudo journalctl -b -k | grep -i "Initialized rknpu"  # either "Initialized" line once it probes
ls -d /sys/bus/platform/drivers/RKNPU/*              # the device it bound to, e.g. fdab0000.npu
sudo cat /sys/kernel/debug/rknpu/version
```

If the driver bound and the scan still misses the node, open it directly:

```sh
ROCKET_DEV=/dev/dri/renderD129 sudo -E ./your-binary
```

<details>
<summary>Why two other common checks do not answer this</summary>

`modinfo rknpu` reports the driver the kernel was built with. On a BSP kernel it reports
`(builtin)` whichever way the probe went, so it cannot tell you the device is there.

`/sys/class/drm/renderD*/subsystem/version` reports the DRM core's version,
`drm 1.1.0 20060810`. It is identical for every node on every machine and names no driver.

The three commands above report the device itself.

</details>

### Allocation refused

A test failure that names an allocation is an [IOVA window](#terms) that cannot serve the
request, and the arithmetic is unaffected. The `rknpu` driver maps every buffer, from every
process, into one IOVA window. The suite's largest shapes ask for single buffers of a hundred
megabytes and more.

Confirm it:

```sh
sudo journalctl -b -k | grep -i iova
```

A reboot resets the window. The same shape generally passes with more workers, which splits the
buffer into pieces the window can still serve. Keep `RKNPU_IOVA_TIGHT`
at its default of `1`, which stops a workload from consuming window space permanently
([`RKNPU_IOVA_TIGHT`](#rknpu_iova_tight) has the detail).

<details>
<summary>How fast the window fragments</summary>

The window fragments over a board's uptime, so a large contiguous request can be refused while
smaller ones still fit. A freshly booted board serves about 3.9 GB in buffers of any size from
16 MB to 256 MB. The same board a day later served 35 buffers of 16 MB, one of 128 MB and none of
192 MB [HW sweep].

`librocketnpu`'s flash attention treats such a refusal as transient: it halves its group of
heads and retries. Elsewhere a refusal fails the call.

</details>

## Running whisper.cpp

Steps 1 through 4 leave a `librocketnpu` that reaches the NPU through the `rknpu` driver. A
frontend links that library and uses the same path, so a frontend built the ordinary way runs on
the NPU unmodified. Steps 5 through 8 take a BSP board to stock `whisper.cpp` running its encoder
on the NPU. They add one repository to the two above:
[`ggml-rocket`](https://github.com/gregordinary/ggml-rocket), a `ggml` backend that an unmodified
host loads through `GGML_BACKEND_PATH`.

### 5. whisper.cpp build

Build `whisper.cpp` with a shared, loadable `ggml`:

```sh
git clone https://github.com/ggml-org/whisper.cpp
cmake -S whisper.cpp -B whisper.cpp/build \
      -DGGML_BACKEND_DL=ON -DBUILD_SHARED_LIBS=ON \
      -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16
cmake --build whisper.cpp/build -j
./whisper.cpp/models/download-ggml-model.sh base.en
```

`GGML_CPU_ARM_ARCH` is required here, because `GGML_BACKEND_DL=ON` forces `GGML_NATIVE=OFF`.
The CPU backend then builds generic armv8 kernels without dotprod and fp16. Such a build runs at
a fraction of the CPU's speed and inflates every NPU-against-CPU comparison taken afterwards. The
run's own `system_info` line is the check: it reports `DOTPROD = 1 | FP16_VA = 1` when the flag
took effect.

For a quantized model rather than the F16 `.en` models, add `-DGGML_CPU_REPACK=OFF`. Offload
requires the weight in its original layout, so a repacked quantized weight runs on the CPU and
the model completes with the NPU idle.

### 6. Backend build

Build the backend against that host's own bundled `ggml`:

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

`HOST_DIR` and `GGML_LIB_DIR` point at the host application's own bundled `ggml`, and the backend
must be built against it. A `.so` built against a different `ggml` checkout registers no device.
The host's loader refuses a different `GGML_BACKEND_API_VERSION`, and the backend refuses a host
whose op numbering moved.

`ROCKETNPU_DRIVER_NAME` is the name a frontend prints in its device listing, so it shows which
kernel path is in use. Left unset, an external build reports `external submit provider`.

`ggml-rocket` builds its own copy of `librocketnpu` from `ROCKETNPU_DIR` and passes the two
provider settings down to it, so they are repeated here. The build from step 3 is the one the
test suite in step 4 checks.

> [!CAUTION]
> Without `ROCKETNPU_DIR`, an installed `rocketnpu` package (a `cmake --install`, typically under
> `/usr/local`) takes precedence, and a stale install builds the backend against a driver library
> nobody named. Set `ROCKETNPU_DIR` explicitly. The configure log names which of the two the build
> used.

### 7. A stock `whisper-cli` run

```sh
GGML_BACKEND_PATH=$PWD/build-ggml-rocket/libggml-rocket.so ROCKET_KACC=1 \
  ./whisper.cpp/build/bin/whisper-cli \
  -m whisper.cpp/models/ggml-base.en.bin -f audio.wav
```

Run with `ROCKET_KACC=1`. Under `sudo`, use `sudo -E`: the plain form starts a fresh environment
without `GGML_BACKEND_PATH`, and the run then loads no backend.

### 8. Offload check

Three lines show that the NPU ran. The first two report the device found and selected:

```
load_backend: loaded ROCKET backend from .../libggml-rocket.so
whisper_backend_init: using ROCKET backend
```

`ROCKET_MM_PROFILE=1` adds the third, which reports work reaching the device:

```
ROCKET profile total(ms): pack=<ms> (packA=<ms> packB=<ms>) gen=<ms> sync=<ms> submit=<ms> wait=<ms> read=<ms> acc=<ms>  over <n> job-batches
```

> [!CAUTION]
> An op the backend declines runs on the CPU and still produces a complete, correct transcript.
> A run that prints the first two lines and not the third ran its encoder on the CPU.

### Performance

With the encoder on the NPU, whisper.cpp uses fewer CPU core-seconds, and the saving is larger
than the wall-clock speedup. The process blocks on the NPU with its threads idle, so the CPU time
freed exceeds the wall-clock time saved. On a 30 s clip, `base.en` runs 1.10x faster in
wall-clock time and uses 17.4% fewer CPU core-seconds. A wall-clock ratio quoted as a CPU saving
is wrong in either direction, depending on the clip.

Two measurements follow, and they answer different questions. In the first, each clip runs in
its own process, so the model load is inside the figure. Across `tiny.en`, `base.en` and
`small.en` at 3, 10, 30, 60 and 120 s, the NPU saves 16-50% of the CPU core-seconds.

The saving rises with model size, and it is largest on short clips. Whisper pads every window to
30 s whatever the audio holds, so a 3 s utterance costs a full encode and decodes almost nothing.
The encoder is the part that runs on the NPU.

The realtime factor, seconds of audio per second of wall-clock time, is 3-33x for `tiny.en`,
1.8-12.9x for `base.en` and 0.7-5.8x for `small.en` with the NPU. The clips below realtime are
the short ones, where the fixed encode and the model load dominate.

The second measurement is the form a service has: the model loaded once, and the cost taken per
utterance of conversational audio. CPU core-seconds per utterance:

| Model | Clip | CPU only | NPU | Saved |
|---|---|---|---|---|
| `tiny.en` | 3 s | 3.03 | 1.93 | 36% |
| `tiny.en` | 10 s | 3.39 | 2.45 | 28% |
| `tiny.en` | 30 s | 4.38 | 3.11 | 29% |
| `base.en` | 3 s | 6.75 | 3.82 | 43% |
| `base.en` | 10 s | 8.37 | 4.89 | 42% |
| `base.en` | 30 s | 9.44 | 6.17 | 35% |

The model load is most of the difference between the two measurements. It is 129 ms for
`tiny.en`, 173 ms for `base.en` and 333 ms for `small.en`, once per process. A CLI started per
transmission incurs it every time and loses much of the saving, so keep the model loaded.

**Conditions:** RK3588 (Turing RK1), the `rknpu` 0.9.8 driver through this provider, NPU at the
600 MHz request, measured 2026-08-24. whisper.cpp ran 4 threads on the A76 cores
(`taskset -c 4-7`), CPU governor `performance`, `ROCKET_KACC=1`, warm.

**Not characterized:** the NPU's physical clock. Above 200 MHz it follows the rail voltage, which
the `rknpu` driver sets per operating point.

> [!NOTE]
> Discard the warm-up run of a measurement: a cold run measures about 15% low. On a stock BSP
> board, DDR also runs under `dmc_ondemand`, which slows a CPU-only run and not an NPU one. Pin
> `/sys/class/devfreq/dmc` to `performance`, as well as the CPU governor, before comparing the
> two.

#### Evidence

Both configurations of every comparison come from one binary, run interleaved with the warm-up
run discarded. The CPU-only configuration is the same build with every op declined by the
backend.

### Agreement with the CPU

The NPU encoder computes in fp16 where the CPU's computes in fp32, so the two transcripts are
close rather than bit-identical. Whether a difference reaches the transcript depends on the
audio. Both configurations are deterministic, and every repetition within one is byte-identical,
so a difference between them comes from the arithmetic and not from a race.

On clean read speech the transcripts are identical, for all three models at every length tested.
On degraded audio they are not. On a two-minute clip of hard conversational speech, `base.en`
differs by 66 word-level edits, a 153-word NPU transcript against a 169-word CPU one. The
differences run in both directions, and neither transcript is ground truth: both are plausible
readings of ambiguous overlapping speech.

The divergence compounds with length rather than staying local. A difference early in an
autoregressive decode changes the prompt context for every window after it.

> [!NOTE]
> Radio and telephony audio is degraded audio. Test agreement on audio like the deployment's: a
> benchmark on clean read speech shows identical transcripts and does not test the degraded case.

### Running as a service

A transcription service keeps `whisper-server` loaded and posts it audio in chunks. OpenWebRX+
(1.2.123 and later) does that. Every `chunkSeconds` of wall-clock time, 20 s by default, it posts
the squelch-gated audio that arrived, as one 12 kHz WAV. It sends no other request field and
reads `text` from the reply.

The server's command-line defaults therefore decide every decode parameter, because the client
sends none. Every request also costs one full encode, because whisper pads a window to 30 s
whatever the chunk holds.

`whisper-server` is built beside `whisper-cli` in step 5. Start it the way step 7 starts the CLI,
pinned to the A76 cluster:

```sh
GGML_BACKEND_PATH=$PWD/build-ggml-rocket/libggml-rocket.so ROCKET_KACC=1 \
  taskset -c 4-7 ./whisper.cpp/build/bin/whisper-server \
  -m whisper.cpp/models/ggml-small.bin -t 4 -nt -ac 1200 -sns \
  --host 127.0.0.1 --port 8080
```

Four settings decide the cost, and each is a rule rather than a tuned number:

- **`-ac` (audio context).** Set it to 50 per second of the chunk plus 4 s, so the encoder covers
  the audio and the silence after it. A 20 s chunk takes 1200, and a 30 s chunk keeps the default
  of 1500, the whole window. Set to the chunk length itself, `-ac` halves the
  encode and breaks the decoder. Every chunk transcribes correctly to the audio's end and then runs
  on into a loop. The model never sees the silence that closes an utterance.
- **`-nt` (no timestamps).** It saves 12% of the CPU core-seconds at equal accuracy, and a 30 s
  chunk requires it. With timestamps on, a chunk whose last timestamp falls short of the audio's
  end has its remainder encoded again as a second window. A 30 s chunk is cut mid-word at the
  window's edge, so that happens on most of them. Plain 30 s chunks therefore cost 13% more CPU
  per second of audio than 20 s ones.
- **`temperature_inc=0`, sent as a request field.** It turns off the temperature fallback. The
  fallback re-decodes a window the decoder is unsure of up to five more times, at about three
  times a normal request's cost. The server's `-nf` flag is parsed and never applied. With `-nt` the
  fallback rarely fires. Turning it off costs a small accuracy margin on the chunks it was
  re-decoding, so the choice is a judgment call.
- **`-sns` (suppress non-speech tokens).** It costs nothing and improves accuracy on every channel
  condition measured. On audio whisper cannot parse, it otherwise emits runs of bracket tokens and
  can loop on them for hundreds of tokens.

Where the client cannot add a field, the server flag is one line away from working, in
`examples/server/server.cpp`:

```diff
-            wparams.temperature_inc  = params.temperature_inc;
+            wparams.temperature_inc  = params.no_fallback ? 0.0f : params.temperature_inc;
```

The table gives CPU core-seconds per second of channel time for `ggml-small`, on read speech
through an SSB-shaped channel at 15 dB SNR. It is relative to the server's defaults on 20 s chunks
with the NPU, which use 0.58x the CPU-only configuration's core-seconds. WER is against the
LibriSpeech reference, on clean speech and on the 15 dB channel, and the chunk cuts set its
minimum:

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

The 20 s rows keep the latency and save 22-23% of the CPU core-seconds at the same or better
accuracy. The 30 s row saves 26%, with the best clean-speech accuracy and a two- to five-point
loss on the noisy channel, at 30 s of latency. Averaged over three channel conditions (clean,
15 dB and 5 dB), the `-sns` row measures 0.70 and the 30 s row 0.65.

Two changes do not help:

- **A quantized model.** `ggml-small-q8_0` takes 10% more CPU than F16. whisper.cpp leaves its
  weight tensors unnamed, which keeps the quantized encoder off the backend's resident route, so
  every request dequantizes the weights again.
- **`-t 2`.** It saves 12% of the CPU core-seconds and makes each request half again as long,
  with the same transcript. The saving costs latency.

**Conditions:** RK3588 (Turing RK1), the mainline `rocket` driver, NPU at ~816 MHz (the 600 MHz
request), whisper.cpp master `eacbd82`, 4 threads on the A76 cores, CPU governor `performance`,
`ROCKET_KACC=1`. Measured 2026-09-03 and 2026-09-04. The server is billed by its own CPU time per
request.

These settings are host-side, and the ranking holds through this provider. A second RK1 ran
`6.1.172-vendor-rk35xx`, the `rknpu` 0.9.8 driver and whisper.cpp `d09f61a`. There, each
configuration's ratio to the default NPU configuration is within 0.03 of the mainline one
[HW sweep 2026-09-26].

#### Evidence

The configurations were run in two passes with their order rotated. A second speaker's chapter
gave the same ranking. On the second RK1, the NPU ran at its 1000 MHz operating point, with DDR
pinned at 2112 MHz.

## Other frontends

Three other frontends reach the NPU by the same path: each links `librocketnpu`, which steps 1
through 4 have already pointed at the `rknpu` driver. Those steps are unchanged in each case, and
only the frontend build differs.

[`ort-rocket`](https://github.com/gregordinary/ort-rocket) is an ONNX Runtime execution provider.
Its build takes the same `ROCKETNPU_*` cache entries as the `tflite-rocket` build below. On
2026-08-26 it ran RF-DETR-nano at 384×384 under ONNX Runtime 1.29.0 on both drivers, and every
output tensor's MD5 matched [HW sweep].

### transcribe.cpp

[`transcribe.cpp`](https://github.com/handy-computer/transcribe.cpp) (MIT) is a `ggml`-based
multi-model speech-to-text library: Whisper, Parakeet, Canary, Granite Speech, Voxtral, SenseVoice
and more. `GGML_BACKEND_PATH` is a `ggml` facility, so transcribe.cpp loads the backend the way
`whisper.cpp` does. Steps 5 and 6 become:

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

`GGML_CPU_ARM_ARCH` is required for the same reason as in step 5. `GGML_LIB_DIR` is this host's
`build/ggml/src` rather than its `build/bin`, and the CPU module is found separately.

Two defects in transcribe.cpp's dynamic-backend loading need a workaround. Neither is in this
provider, and neither reports itself as what it is:

- The CLI registers dynamic backends only on its `--list-devices` path. A shared build then loads
  no backend at all when it transcribes, and stops with `whisper: failed to initialize CPU
  backend`. The fix is one call to `transcribe_init_backends_default()` before the model load in
  `examples/cli/main.cpp`.
- The backend scan reads only the directory the library itself sits in. The Arm CPU module is a
  plain `libggml-cpu.so` elsewhere in the build, so run `cp build/bin/libggml-cpu.so build/src/`.
  Without it only the NPU registers, and the run fails as above.

Then run it, with the module search path pointed at the build and the NPU supplied the same way:

```sh
cd transcribe.cpp
LD_LIBRARY_PATH=$PWD/build/ggml/src:$PWD/build/bin:$PWD/build/src \
GGML_BACKEND_PATH=../build-gr-transcribe/libggml-rocket.so ROCKET_KACC=1 \
  ./build/bin/transcribe-cli -m <model>.gguf audio.wav
```

Its device line is `whisper: using accel backend: ROCKET`. `ROCKET_MM_PROFILE=1` adds the same
profile line as in step 8, which remains the only evidence that work reached the device.
`--list-devices` shows `kind=accel`, which places the NPU ahead of the CPU in its scheduler.

| Configuration | Wall-clock time | CPU core-seconds |
|---|---:|---:|
| NPU | 1.88 s | 9.00 |
| CPU only | 2.97 s | 21.70 |

The NPU run is 1.58x faster in wall-clock time and uses 58.5% fewer CPU core-seconds, with
identical transcripts. Both runs use this CLI's default of every core, the Cortex-A55 cores
included. Its core-second figures are therefore roughly twice `whisper-cli`'s at four threads,
and the ratio is not comparable to the tables above.

**Conditions:** RK3588 (Turing RK1), `6.1.115-vendor-rk35xx`, the `rknpu` 0.9.8 driver through
this provider, `whisper-base.en-Q8_0`, an 11 s clip, every CPU core.

**Not characterized:** this comparison with both runs pinned to the A76 cores.

#### Evidence

Each figure is the median of three interleaved runs.

### tflite-rocket

[`tflite-rocket`](https://github.com/gregordinary/tflite-rocket) is a TensorFlow Lite external
delegate: a `.so` that any TFLite host loads at runtime and runs against an unmodified `.tflite`
model. It links `librocketnpu` directly, so steps 5 onward become one build:

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
`tensorflow/lite/core/c/{common,builtin_op_data}.h` plus `tensorflow/lite/builtin_ops.h`. The
build also produces `libtflite_cshim.so`, which supplies the two TFLite C-API symbols the delegate
binds at `dlopen`. The classic `tflite_runtime` and full `tensorflow` wheels export them. LiteRT
(`ai_edge_litert`, the wheel on recent Python) keeps them internal, so under LiteRT, `LD_PRELOAD`
the shim. Without it the delegate stops at `undefined symbol: TfLiteIntArrayCreate`.

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

> [!CAUTION]
> `OpResolverType.BUILTIN_WITHOUT_DEFAULT_DELEGATES` is required. It holds XNNPACK back, so the
> convolutions are offered to the external delegate first. Under the default resolver, XNNPACK
> claims them and the model runs entirely on the CPU while its log still reports a delegate. The
> delegate's `profile=1` option prints each convolution's route, which shows where it ran.

The two drivers give byte-identical detector outputs. On 2026-08-26 an MD5 over every output
tensor of SSDLite-MobileDet and EfficientDet-Lite0 matched between the `rknpu` driver and the
mainline `rocket` driver [HW sweep]. It matched in the CPU-only, `native_int8=1` and
`native_int8=0` configurations, with every on-NPU auxiliary route enabled, and with
`nchw_resident=1`.

Four concurrent detection processes, the form a multi-camera deployment takes, returned the
single-process output hash on both drivers. Their aggregate throughput was 3.51x one process's
here, against 3.61x on the mainline driver. Warm single-inference latency was within 2%: 202.1 ms
against the mainline driver's 197.9 for MobileDet, and 299.6 against 293.6 for
EfficientDet-Lite0. The workload is host-bound, and the two boards' CPUs differ by as much on
their own.

**Conditions:** two RK3588 (Turing RK1) boards, one per driver, both NPUs at the 600 MHz request.
The delegate was built on 2026-08-26 from the same sources on both.

**Not characterized:** the NPUs' physical clocks, which follow each kernel's rail voltage, so the
latency comparison is not clock-matched. Also the delegate's current `native_int8` defaults, which
requantize on chip through `rocket_conv2d_int8_q`, `rocket_conv2d_int8_q_perc` and the uint8 and
per-axis depthwise entries, through this provider.

## Environment variables

| Variable | Default | Effect |
|---|---|---|
| `ROCKET_DEV` | probe by driver name | Opens a specific node, for a board with more than one NPU or a test rig. |
| `RKNPU_CORE_MASK` | `0` (the driver schedules cores) | Pins submits to one core: `1`, `2` or `4`. The provider logs any other value and lets the driver schedule. |
| `ROCKET_BATCH_SUBMIT` | `1` (chained) | Read by `librocketnpu`, whose default on this provider is a [chained batch](#terms). `0` issues one submit per register program. |
| `RKNPU_IOVA_TIGHT` | `1` | `0` maps buffers through the kernel's generic IOVA path, which consumes IOVA window space permanently. Requires `rknpu` 0.9.7. [`RKNPU_IOVA_TIGHT`](#rknpu_iova_tight) has the detail. |
| `RKNPU_BO_SENTINEL` | `0` | Fills each new buffer with the given byte, so that an all-zero readback tells "the device wrote zeros" from "the device never wrote". |

### `RKNPU_IOVA_TIGHT`

`RKNPU_IOVA_TIGHT=1` keeps a workload from consuming the IOVA window. The `rknpu` driver maps
every buffer, from every process, through one IOMMU domain. Setting `RKNPU_IOVA_TIGHT=0` selects the
kernel's generic mapping path, which consumes window space that the driver's path cannot reuse.
The loss outlives the process. A reboot recovers it. So does an allocation on the generic path
that the window refuses, which makes the kernel flush the space it cached [HW sweep 2026-08-26].

A single 2048-token prefill (Llama-3.2-3B F16, 153 s) on the generic path permanently costs the
window 5-11 of the 31 buffers of 128 MiB it can serve. With `RKNPU_IOVA_TIGHT=1` the same run
costs none. Throughput is the same either way: 39.90-41.39 tokens/s at 2048 tokens and 67.57 at
512 in both configurations. The setting preserves allocation headroom and does not change speed
[HW sweep].

> [!CAUTION]
> A stock RKNN userspace maps its buffers on the generic path. Beside one, this process can meet
> `-ENOMEM` while the window still holds space it cannot use. Retry a large refused allocation at
> a smaller size. `librocketnpu` does that only in its flash attention.

> [!NOTE]
> A board with unknown uptime can already have a fragmented window. Check the window before
> trusting an allocation-sensitive measurement on one.

The kernel-side mechanism, and the measurements that isolated it, are in the comment above
`rknpu_iova_tight_flag()` in `src/rknpu_provider.c`.

## Driver behavior

Reference for readers tuning the defaults above or changing the provider. The `rknpu` uAPI
differs from the mainline one in buffer identity, job structure and submit semantics. The table
states each behavior and how the provider handles it.

| Behavior | The `rknpu` driver | The provider |
|---|---|---|
| Buffer identity | `MEM_SYNC` keys on the memory object's address, not its handle | `rocket_bo` has an `obj_addr` field, and the provider passes both |
| Task array | Read through the object's kernel mapping | Allocated with `RKNPU_MEM_KERNEL_MAPPING`, one growable buffer per open file |
| Submit | Blocks until the job retires. `RKNPU_JOB_BLOCK` is 0, and `NONBLOCK` is the flag | The caller's wait on the output is already satisfied on return |
| Multi-program job | One contiguous register-command stream whose trailers link, kicked once | `librocketnpu`'s chained layout, on by default |
| Job scope | One job per submit, and the core is released between jobs | Operand reuse is enabled only for a chained batch |
| Completion | The job's last task supplies `INT_MASK` | Each submit names its terminal block |

### Chained submission and operand reuse

The driver programs `PC_DATA_ADDR` and `PC_DATA_AMOUNT` from the first task, writes the task
count into `PC_TASK_CONTROL`, and kicks once. A job of *n* tasks is therefore one contiguous
stream of *n* register programs whose own trailers link to the next. That stream is the layout
`librocketnpu`'s `rocket_chain.c` builds, and the multi-program form this uAPI takes natively.

Each submit is its own job, so the driver takes the next entry off its per-core `todo_list` as
soon as one retires. Another thread's or another process's job can run between two programs.

> [!CAUTION]
> Operand reuse depends on the previous task's operand still being in the CBUF, so it is valid
> only within a chained batch. On an unchained batch with reuse enabled, a job that runs between
> two programs corrupts the result intermittently. The corrupt result is a full, plausible output
> that changes between runs.

`librocketnpu` queries this directly. `rocket_submit_batch_atomic()` reports whether a batch runs
as one job (0 here, 1 on the mainline driver), and `librocketnpu` enables reuse only where it
does. `rocket_batched_submit_native()` reports that the chained layout is native to this uAPI,
and `ROCKET_BATCH_SUBMIT` takes its default from that.

### Naming the completing block

The driver completes a job when the interrupt status collapses to the `INT_MASK` its last task
names. Each submit therefore names the block its job finishes on. A job ending in a DPU write
uses the DPU pair (`0x300`). A pooling job ends on the PPU pair (`0xc00`), which the job flag
`ROCKET_JOB_PPU_DONE` selects.

> [!CAUTION]
> A mask naming a block that finishes earlier retires the job at that point, before the program's
> last stage has written its output. With a `0x3` mask at 64×256×256, hardware elapsed time falls
> from 121 to 79 microseconds. Software must name the program's terminal stage.

#### Evidence

With reuse enabled and an unchained batch, one 128×1024×1024 fp16 matmul at three-way fan-out
produced a corrupt result on 29 of 120 runs. With reuse disabled it produced none in 120 runs,
and chained, none in 80 [HW sweep].

## Matmul throughput

512×3840×4096 fp16 through `librocketnpu`'s resident multicore path:

| Configuration | Best (ms) | GOP/s |
|---|---|---|
| Core 0 pinned, unchained | 68-77 | 208-236 |
| Core 0 pinned, chained | 36.8-39.3 | 409-437 |
| Driver core scheduler, unchained | 47.5-48.3 | 333-339 |
| Driver core scheduler, chained (the defaults) | 26.1-27.7 | 581-617 |
| The same shape through the mainline `rocket` driver, for reference | 23.5-24.4 | 660-685 |

**Conditions:** RK3588 (Turing RK1), the `rknpu` 0.9.8 driver at its 1000 MHz operating point,
which the driver's `rknpu_ondemand` governor holds at idle.

**Not characterized:** the NPU clock of the mainline row.

The two defaults combine. With the driver scheduling cores, `librocketnpu`'s fan-out across the
three cores runs in parallel. The chained layout removes both the extra ioctls and the
operand-reuse restriction.

#### Evidence

Each figure is the best of five repetitions after three warm rounds.

## Measurement functions

`include/rknpu_submit.h` declares three functions that reach measurements the `rknpu` uAPI has
and the mainline one does not:

- `rknpu_last_hw_elapse_ns()`: the hardware elapsed time of the last submit on a file
  descriptor. The driver stamps it as a `ktime` pair around the register-program commit, so it
  spans the register write burst, execution and interrupt latency.
- `rknpu_hw_elapse_total_ns()`: the same time summed over every submit the process has made since
  `rocket_submit_counters_reset()`, with the submit count. A tiled or multicore shape needs the
  sum, because each submit overwrites the per-descriptor value.
- `rknpu_action()`: the raw action ioctl behind the clock, voltage, IOMMU-domain, SRAM and
  DMA-counter queries.

Query availability before planning a measurement. The driver configurations differ by part:

| Measurement | Parts whose `rknpu` configuration implements it |
|---|---|
| DMA byte counters | RK3576, RK356x, RK3562, RV1106, RV1126B |
| NBUF pool | RK3576, RK3562, RV1126B |
| Bandwidth QoS | RK356x |
| SRAM pool | Any part whose device tree assigns SRAM to the NPU |

On the RK3588 the driver configuration leaves the counter registers unset and the NBUF size at
zero. A counter query returns 0 and leaves the value as the caller passed it, and the driver logs
`Get rw_amount is not supported on this device!` [source-confirmed: BSP `rknpu_drv.c`, 0.9.8]. Per-submit hardware elapsed time, the clock and voltage queries, and the IOMMU domain
identifier are available there. The header states each function's bounds.

## Validation

Measured on an RK3588 (Turing RK1), Armbian `6.1.172-vendor-rk35xx`, `rknpu 0.9.8 20240828`, NPU
at `/dev/dri/renderD129`, on 2026-09-26.

The steps above were followed as written, from fresh clones:

| Component | Version | Result |
|---|---|---|
| `rknpu-submit` | `db7c3ab` | Configures and builds with no errors, and `provider_seam` passes |
| `rocket-userspace` | `c7e7c47` | Builds. The full suite passes: 76 of 97 run, and 21 skip as step 4 describes |
| `ggml-rocket` | `cfcc0f5` | Builds against the provider, and `libggml-rocket.so` loads |
| `whisper.cpp` | master `d09f61a` | `whisper-cli` prints all three lines of step 8, and its `samples/jfk.wav` transcript matches the CPU run's |
| `transcribe.cpp` | `c6a9257` | Selects the NPU as an `accel` device and offloads, with the two workarounds its section names |
| `tflite-rocket` | unrecorded | `convert_test` at 220 cases and 0 failures. Six driver-level probes pass, including a 1494-shape CBUF bank-slack sweep at zero error. Detector outputs byte-identical to the mainline driver's |

The `transcribe.cpp` and `tflite-rocket` rows are from an earlier walk on
`6.1.115-vendor-rk35xx`, with the same driver.

`ctest` in `build-rknpu`, the provider's own build directory, runs `provider_seam`. It needs no
hardware. It checks that this provider defines every seam function the built-in mainline provider
defines. Linking a static archive does not check that. It is registered only against
a `librocketnpu` source checkout, because the symbol list lives there.

## The rocket NPU stack

This provider is one part of an open source stack for Rockchip NPUs. The stack is a driver
library, three frontends, this submit provider for the BSP kernel, and a set of optional kernel
patches:

- **[`rocket-userspace`](https://github.com/gregordinary/rocket-userspace)** (`librocketnpu`): the
  userspace driver, matmul, and on-NPU op library. It is the library this provider plugs into.
- **[`ggml-rocket`](https://github.com/gregordinary/ggml-rocket)**: a ggml backend `.so` for
  `llama.cpp` and `whisper.cpp`, linking the same library.
- **[`tflite-rocket`](https://github.com/gregordinary/tflite-rocket)**: a TFLite external delegate
  for detection models, linking the same library.
- **[`ort-rocket`](https://github.com/gregordinary/ort-rocket)**: an ONNX Runtime execution
  provider for transformer encoders and per-layer convolutions, linking the same library.
- **`rknpu-submit`**: this provider, which runs `librocketnpu` on the `rknpu` BSP kernel driver
  instead of the mainline `rocket` driver.
- **[`patches`](https://github.com/gregordinary/patches)** (`rocket/` scope): optional out-of-tree
  patches for the mainline `rocket` module, for clock, voltage and IOMMU. A BSP kernel does not
  need them.

## License

GPL-3.0-or-later.
