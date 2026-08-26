// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The rknpu-submit authors
/*
 * rknpu_provider.c — librocketnpu's submit seam, implemented over the vendor `rknpu`
 * BSP driver instead of the mainline `accel/rocket` one.
 *
 * The seam is include/rocket_npu.h and nothing else: device open/close, BO
 * alloc/free, cache maintenance, submit, and the capability + counter queries. Every
 * line ABOVE it — the regcmd encoders, the tiling, the op library, graph/, the
 * frontends — is silicon-shaped rather than driver-shaped and is compiled unchanged.
 * That the same register program produces byte-identical output through both drivers
 * is measured, not assumed: FNV-1a `0a003896b051d7cb` at 4x32x16 and
 * `cacd57a95d6d0441` at 64x256x256, matching on both kernels and against the host
 * reference. [HW sweep]
 *
 * This links no vendor blob. `librknnrt.so` is not in the path and not required; the
 * target is the GPL kernel driver's ioctl surface, which takes the same CNA->CORE->DPU
 * register programs the encoders already emit.
 *
 * ---- Three places the two uAPIs genuinely differ ----------------------------
 *
 * 1. BOs are keyed on an OBJECT ADDRESS, not just a GEM handle. MEM_DESTROY and
 *    MEM_SYNC both take `obj_addr`, so rocket_bo carries the field (inert on the
 *    mainline provider, which leaves it 0).
 *
 * 2. The task array lives in DEVICE memory. rknpu_submit carries `task_obj_addr`, and
 *    the driver reads the array through the object's KERNEL mapping — so the task BO
 *    must be created with RKNPU_MEM_KERNEL_MAPPING or the driver dereferences NULL.
 *    That is a crash, not a refusal. The seam's `scratch` argument is host memory and
 *    cannot serve; this provider keeps its own per-fd task BO and grows it on demand,
 *    which is invisible through the unchanged signature.
 *
 * 3. SUBMIT BLOCKS. RKNPU_JOB_BLOCK is 0 and NONBLOCK is the flag, so this ioctl
 *    returns only once the job has retired. The mainline SUBMIT is asynchronous and
 *    the caller waits by PREP_BO'ing the output. Blocking is the STRONGER guarantee,
 *    so every caller written against the async contract is still correct here — a
 *    later PREP_BO simply finds the work already done. What it costs is submit
 *    pipelining, which is a throughput property and not a correctness one.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rocket_npu.h"
#include "rocket_hw_profile.h"   /* rocket_hw_current(), resolved once at open */
#include "rknpu_uapi.h"
#include "rknpu_submit.h"

#ifndef RKNPU_LOGE
#define RKNPU_LOGE(...) fprintf(stderr, "[rknpu-submit] " __VA_ARGS__)
#endif

#define RKNPU_PAGE 4096u
static size_t page_up(size_t n) { return (n + RKNPU_PAGE - 1) & ~(size_t)(RKNPU_PAGE - 1); }

/* ============================================================================
 * SECTION — per-fd device state
 *
 * The seam hands round a bare int fd, but two things about a device do not fit in
 * one: whether it is the DRM render node or the misc /dev/rknpu node (the ioctl
 * MAGIC differs — both are compiled from the same handler, so either may be what a
 * given build exposes), and the task BO this provider owns on that fd. A table keyed
 * on the fd carries both.
 *
 * INDEXED BY THE FD, not searched. What this table sizes is not how many DEVICES
 * exist — it is how many fds one process holds open at once, and the library opens
 * one per worker: rocket_matmul_fp16_mt takes up to 8, flash attention runs 8 heads
 * in parallel, and the caller's own fd sits on top of all of them. An 8-entry table
 * refused the 9th with -EMFILE, which is loud rather than wrong but still stops a
 * gate (flash_attn_rocket at mt=8). Indexing by fd also removes the free-slot search
 * and the fd-reuse ambiguity that came with it.
 *
 * Two levels so growth never moves an entry a lock-free reader is holding: a fixed
 * array of block pointers, each block allocated once on demand and never freed. The
 * cap is 4096 fds, four times the usual RLIMIT_NOFILE soft limit, and a request past
 * it is refused rather than silently mis-served.
 * ==========================================================================*/

#define RKNPU_DEV_BLOCK  64
#define RKNPU_DEV_BLOCKS 64
#define RKNPU_MAX_FD     (RKNPU_DEV_BLOCK * RKNPU_DEV_BLOCKS)

struct rknpu_bo_priv { uint32_t handle; uint64_t obj_addr; uint64_t dma_addr; size_t size; void *ptr; };

struct rknpu_dev {
    int  fd;             /* -1 = free slot */
    int  is_drm;         /* 1 = DRM render node ('d' magic), 0 = /dev/rknpu ('r') */
    struct rknpu_bo_priv taskbo;   /* the rknpu_task[] array, grown on demand */
    uint32_t taskbo_cap;           /* how many rknpu_task entries it holds     */
    int64_t  last_hw_elapse_ns;    /* rknpu_submit.hw_elapse_time, written back */
    /* The task BO is per-DEVICE state that a submit rewrites in place, and the
     * mainline provider has no equivalent — there the kernel copies the task array
     * from a caller-owned pointer, so two threads on one fd never share a buffer.
     * This provider introduced the shared resource and therefore owes the lock.
     *
     * Without it the failure is a plausible wrong surface, not a crash: one thread
     * overwrites the array between another's fill and its ioctl, and the second job
     * runs the first's program. Measured on the multi-threaded FFN and flash-attention
     * paths as a cosine similarity of 0.88-0.94 that moved between runs, while every
     * single-threaded gate was exact. */
    pthread_mutex_t submit_lock;
};

static struct rknpu_dev *_Atomic g_blocks[RKNPU_DEV_BLOCKS];
static pthread_mutex_t  g_devs_lock = PTHREAD_MUTEX_INITIALIZER;

/* The entry for `fd`, or NULL. `grow` allocates the block if this is an open; a
 * lookup never allocates, so a stale fd reads NULL instead of manufacturing state. */
static struct rknpu_dev *dev_slot(int fd, int grow)
{
    if (fd < 0 || fd >= RKNPU_MAX_FD) return NULL;
    int b = fd / RKNPU_DEV_BLOCK, i = fd % RKNPU_DEV_BLOCK;
    struct rknpu_dev *blk = atomic_load(&g_blocks[b]);
    if (!blk) {
        if (!grow) return NULL;
        struct rknpu_dev *fresh = calloc(RKNPU_DEV_BLOCK, sizeof *fresh);
        if (!fresh) return NULL;
        for (int k = 0; k < RKNPU_DEV_BLOCK; k++) fresh[k].fd = -1;
        pthread_mutex_lock(&g_devs_lock);
        blk = atomic_load(&g_blocks[b]);
        if (!blk) { blk = fresh; atomic_store(&g_blocks[b], blk); fresh = NULL; }
        pthread_mutex_unlock(&g_devs_lock);
        free(fresh);                       /* lost the race; the winner's block stands */
    }
    return &blk[i];
}

static struct rknpu_dev *dev_find(int fd)
{
    struct rknpu_dev *d = dev_slot(fd, 0);
    return (d && d->fd == fd) ? d : NULL;
}

/* The ioctl request word. The DRM node encodes on 'd' + DRM_COMMAND_BASE; the misc
 * node on 'r' with the bare command number. Same handler behind both. */
static unsigned long ioc(const struct rknpu_dev *d, unsigned nr, size_t sz)
{
    return d->is_drm ? (unsigned long)_IOC(_IOC_READ | _IOC_WRITE, RKNPU_DRM_IOCTL_BASE,
                                           RKNPU_DRM_COMMAND_BASE + nr, sz)
                     : (unsigned long)_IOC(_IOC_READ | _IOC_WRITE, 'r', nr, sz);
}

/* ============================================================================
 * SECTION — submit counters (the seam's own, process-wide)
 * ==========================================================================*/

/* Relaxed atomics rather than plain counters: the seam documents these as not
 * thread-safe to READ meaningfully, which is a statement about the value being a
 * moving target, not a licence to race the increments themselves. */
static _Atomic uint64_t g_ioctls, g_tasks;
/* Summed device time, for the sweeps that have more than one kick. The per-fd
 * last_hw_elapse_ns below is overwritten by every submit, so a tiled or multicore
 * matmul reports only its final one; this pair is what a multi-submit shape needs.
 * A job the driver never committed writes back 0 (hw_commit_time == 0), which is not
 * a zero-length submit -- those are excluded from both sums rather than counted. */
static _Atomic uint64_t g_hw_ns, g_hw_submits;
static void count_submit(uint32_t n, int64_t hw_ns)
{
    atomic_fetch_add_explicit(&g_ioctls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_tasks, n, memory_order_relaxed);
    if (hw_ns > 0) {
        atomic_fetch_add_explicit(&g_hw_ns, (uint64_t)hw_ns, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_hw_submits, 1, memory_order_relaxed);
    }
}

uint64_t rocket_submit_ioctl_count(void)
{ return atomic_load_explicit(&g_ioctls, memory_order_relaxed); }
uint64_t rocket_submit_task_count(void)
{ return atomic_load_explicit(&g_tasks, memory_order_relaxed); }
void rocket_submit_counters_reset(void)
{
    atomic_store_explicit(&g_ioctls, 0, memory_order_relaxed);
    atomic_store_explicit(&g_tasks, 0, memory_order_relaxed);
    atomic_store_explicit(&g_hw_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_hw_submits, 0, memory_order_relaxed);
}

/* ============================================================================
 * SECTION — BO primitives (private; the seam's rocket_bo_* wrap these)
 * ==========================================================================*/

/* RKNPU_IOVA_TIGHT=0 restores the kernel's generic IOVA mapping path (see
 * RKNPU_MEM_IOMMU_LIMIT_IOVA_ALIGNMENT in rknpu_uapi.h). ON by default, because the
 * generic path LEAKS the shared IOMMU domain and this one does not.
 *
 * What it buys, llama.cpp pp2048 on Llama-3.2-3B F16 [HW sweep, rknpu 0.9.8, 2026-08-25]:
 * one 153 s run with the flag off costs the process-wide domain 5-11 of its 31 128 MiB
 * buffers, permanently -- the loss outlives the process and only a reboot resets it.
 * With the flag on the same run costs ZERO: four such arms, interleaved around the
 * leaking ones, left all four size counts byte-identical. Throughput is the same either
 * way (39.90-41.39 t/s across five runs), so this is headroom and not speed.
 *
 * Do NOT explain the flag by allocation size. The generic path's power-of-two rounding
 * applies only below 128 KiB on kernels >= 6.1 (IOVA_RANGE_CACHE_MAX_SIZE in iova.c), and
 * allocating one size until refusal returns identical counts AND identical addresses on
 * both routes at 16 / 32 / 32.03 / 48 / 64 / 128 / 192 MiB. No static probe separates
 * them; the mechanism is unidentified and lives in the real workload's allocation
 * pattern. It is not the error path either -- all five runs reported zero kernel
 * allocation failures on both routes.
 *
 * It needs the BO prefill below to be correct; without that it costs two gates.
 * With it, 93 of 93 twice, and no measurable cost at pp512 (67.57 t/s in both arms over
 * four interleaved rounds with the board to itself). */
static uint32_t rknpu_iova_tight_flag(void)
{
    static _Atomic int cached = -1;
    int v = cached;
    if (v < 0) {
        const char *e = getenv("RKNPU_IOVA_TIGHT");
        v = (e && strtoul(e, NULL, 0) == 0) ? 0
                                            : (int)RKNPU_MEM_IOMMU_LIMIT_IOVA_ALIGNMENT;
        cached = v;
    }
    return (uint32_t)v;
}

static int bo_create(struct rknpu_dev *d, size_t want, uint32_t extra,
                     struct rknpu_bo_priv *b)
{
    struct rknpu_mem_create c;
    struct rknpu_mem_map m;
    size_t mapped = page_up(want ? want : 1);

    memset(b, 0, sizeof *b);
    memset(&c, 0, sizeof c);
    c.size  = mapped;
    /* NON_CONTIGUOUS deliberately — see the note in rknpu_uapi.h. A contiguous
     * request becomes unmappable once CMA runs short, silently and only under load. */
    c.flags = RKNPU_MEM_NON_CONTIGUOUS | RKNPU_MEM_CACHEABLE | RKNPU_MEM_IOMMU |
              RKNPU_MEM_ZEROING | rknpu_iova_tight_flag() | extra;
    if (ioctl(d->fd, ioc(d, RKNPU_MEM_CREATE_NR, sizeof c), &c) < 0) {
        int e = errno;
        RKNPU_LOGE("MEM_CREATE(%zu): %s\n", mapped, strerror(e));
        return -e;
    }
    memset(&m, 0, sizeof m);
    m.handle = c.handle;
    if (ioctl(d->fd, ioc(d, RKNPU_MEM_MAP_NR, sizeof m), &m) < 0) {
        int e = errno;
        struct rknpu_mem_destroy z = { .handle = c.handle, .obj_addr = c.obj_addr };
        RKNPU_LOGE("MEM_MAP(handle=%u): %s\n", c.handle, strerror(e));
        ioctl(d->fd, ioc(d, RKNPU_MEM_DESTROY_NR, sizeof z), &z);
        return -e;
    }
    void *p = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t)m.offset);
    if (p == MAP_FAILED) {
        int e = errno;
        struct rknpu_mem_destroy z = { .handle = c.handle, .obj_addr = c.obj_addr };
        RKNPU_LOGE("mmap(handle=%u, %zu): %s\n", c.handle, mapped, strerror(e));
        ioctl(d->fd, ioc(d, RKNPU_MEM_DESTROY_NR, sizeof z), &z);
        return -e;
    }
    b->handle = c.handle; b->obj_addr = c.obj_addr; b->dma_addr = c.dma_addr;
    b->size = mapped; b->ptr = p;

    /* On the LIMIT_IOVA_ALIGNMENT path a fresh BO is not usable as handed back:
     * RKNPU_MEM_ZEROING's fill is not established from this mapping's point of
     * view, and an op that relies on a zero-filled region reads back a surface
     * that never took the device's writes. Touching every page from the CPU and
     * flushing makes it usable. Measured: with it, conv2d_fp16_rocket and
     * reduce_mean_rocket pass under RKNPU_IOVA_TIGHT=1; without it they return
     * an all-zero surface. The stamp byte does not matter -- 0x00 and 0xA5 both
     * fix it -- so it is the touch and the writeback that count, not the value.
     *
     * RKNPU_BO_SENTINEL=<byte> overrides the fill for diagnosis: an all-zero
     * output is ambiguous here (it reads the same whether the device wrote zeros
     * or never wrote at all) and a non-zero stamp separates those two. */
    {
        const char *e = getenv("RKNPU_BO_SENTINEL");
        int fill = e && *e ? (int)(strtoul(e, NULL, 0) & 0xff) : 0;
        if (e || rknpu_iova_tight_flag()) {
            memset(p, fill, mapped);
            struct rknpu_mem_sync sy;
            memset(&sy, 0, sizeof sy);
            sy.obj_addr = c.obj_addr; sy.offset = 0; sy.size = mapped;
            sy.flags = RKNPU_MEM_SYNC_TO_DEVICE;
            if (ioctl(d->fd, ioc(d, RKNPU_MEM_SYNC_NR, sizeof sy), &sy) < 0)
                RKNPU_LOGE("BO prefill MEM_SYNC(handle=%u): %s\n", c.handle,
                           strerror(errno));
        }
    }
    return 0;
}

static void bo_destroy(struct rknpu_dev *d, struct rknpu_bo_priv *b)
{
    if (!b->handle) return;
    if (b->ptr) {
        if (munmap(b->ptr, b->size) < 0)
            RKNPU_LOGE("munmap(handle=%u, %zu): %s\n", b->handle, b->size, strerror(errno));
        b->ptr = NULL;
    }
    struct rknpu_mem_destroy z = { .handle = b->handle, .obj_addr = b->obj_addr };
    if (ioctl(d->fd, ioc(d, RKNPU_MEM_DESTROY_NR, sizeof z), &z) < 0)
        RKNPU_LOGE("MEM_DESTROY(handle=%u): %s — leaking the object and its IOVA\n",
                   b->handle, strerror(errno));
    b->handle = 0;
}

static int bo_sync(struct rknpu_dev *d, uint64_t obj_addr, uint64_t off, uint64_t len,
                   uint32_t dir)
{
    struct rknpu_mem_sync s;
    memset(&s, 0, sizeof s);
    s.obj_addr = obj_addr; s.offset = off; s.size = len; s.flags = dir;
    if (ioctl(d->fd, ioc(d, RKNPU_MEM_SYNC_NR, sizeof s), &s) < 0) {
        int e = errno;
        RKNPU_LOGE("MEM_SYNC(dir=0x%x, obj=0x%llx, %llu..%llu): %s\n", dir,
                   (unsigned long long)obj_addr, (unsigned long long)off,
                   (unsigned long long)(off + len), strerror(e));
        return -e;
    }
    return 0;
}

/* ============================================================================
 * SECTION — Device open / close
 * ==========================================================================*/

/* Probe by DRM DRIVER NAME, never by node number. On the boards this was written
 * against the NPU is renderD129 and renderD128 is the display subsystem; that
 * ordering is a property of probe order and must not be assumed. ROCKET_DEV forces a
 * specific node for a multi-NPU box or a test rig, matching the mainline provider. */
static int open_node(struct rknpu_dev *d)
{
    const char *forced = getenv("ROCKET_DEV");
    char namebuf[64];

    if (forced && *forced) {
        int fd = open(forced, O_RDWR | O_CLOEXEC);
        if (fd < 0) { RKNPU_LOGE("open(%s): %s\n", forced, strerror(errno)); return -errno; }
        struct rknpu_drm_version v;
        memset(&v, 0, sizeof v); memset(namebuf, 0, sizeof namebuf);
        v.name_len = sizeof namebuf - 1; v.name = namebuf;
        /* A misc node answers no DRM VERSION; that is how the two are told apart. */
        d->is_drm = (ioctl(fd, RKNPU_DRM_IOCTL_VERSION, &v) == 0);
        if (d->is_drm && strcmp(namebuf, "rknpu") != 0) {
            RKNPU_LOGE("%s is DRM driver '%s', expected 'rknpu'\n", forced, namebuf);
            close(fd);
            return -ENODEV;
        }
        d->fd = fd;
        return 0;
    }

    for (int n = 128; n < 144; n++) {
        char p[64];
        struct rknpu_drm_version v;
        snprintf(p, sizeof p, "/dev/dri/renderD%d", n);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        memset(&v, 0, sizeof v); memset(namebuf, 0, sizeof namebuf);
        v.name_len = sizeof namebuf - 1; v.name = namebuf;
        if (ioctl(fd, RKNPU_DRM_IOCTL_VERSION, &v) == 0 && strcmp(namebuf, "rknpu") == 0) {
            d->fd = fd; d->is_drm = 1;
            return 0;
        }
        close(fd);
    }
    int fd = open("/dev/rknpu", O_RDWR | O_CLOEXEC);
    if (fd >= 0) { d->fd = fd; d->is_drm = 0; return 0; }
    RKNPU_LOGE("no rknpu device found (no /dev/dri/renderD12x answers to 'rknpu', "
               "no /dev/rknpu)\n");
    return -ENODEV;
}

int rocket_open(void)
{
    struct rknpu_dev tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.fd = -1;

    int rc = open_node(&tmp);
    if (rc < 0) return rc;

    struct rknpu_dev *slot = dev_slot(tmp.fd, 1);
    if (!slot) {
        RKNPU_LOGE("fd %d is past this provider's per-fd table (cap %d) or the table "
                   "could not grow; refusing rather than silently mis-serving it\n",
                   tmp.fd, RKNPU_MAX_FD);
        close(tmp.fd);
        return -EMFILE;
    }
    /* The slot IS this fd's, and the kernel does not hand the same fd to two opens, so
     * no lock is needed to claim it — only to grow the block, which dev_slot did. */
    *slot = tmp;
    pthread_mutex_init(&slot->submit_lock, NULL);

    /* Resolve the chip profile at the one point every user passes through, matching
     * the mainline provider — so a chip with no profile warns once per process even on
     * a path that never reads it. rocket_sysfs_bound_devices() finds no bound `rocket`
     * driver here and returns 0, and the detect chain falls through to
     * /proc/device-tree/compatible, which reads the part correctly. */
    (void)rocket_hw_current();
    return slot->fd;
}

void rocket_close(int fd)
{
    if (fd < 0) return;
    struct rknpu_dev *d = dev_find(fd);
    if (d) {
        /* BEFORE the close, as on the mainline provider: the task BO's handle belongs
         * to this file and close() destroys it, so a teardown after would free an
         * object that no longer exists. */
        bo_destroy(d, &d->taskbo);
        d->taskbo_cap = 0;
        pthread_mutex_destroy(&d->submit_lock);
        d->fd = -1;
    }
    close(fd);
}

/* ============================================================================
 * SECTION — Buffer objects
 * ==========================================================================*/

int rocket_bo_alloc(int fd, size_t size, rocket_bo *bo)
{
    struct rknpu_dev *d = dev_find(fd);
    struct rknpu_bo_priv b;
    memset(bo, 0, sizeof *bo);
    if (!d) return -EBADF;
    int rc = bo_create(d, size, 0, &b);
    if (rc < 0) return rc;

    bo->handle      = b.handle;
    bo->dma_address = b.dma_addr;
    bo->mmap_offset = 0;         /* MEM_MAP's offset is consumed inside bo_create */
    /* The REQUESTED size, as on the mainline provider: rocket_bo_ensure32's reuse
     * test and the ranged cache ioctls are both written against it, and munmap rounds
     * its length up to the page, so the shorter value still unmaps the whole mapping. */
    bo->size        = size;
    bo->ptr         = b.ptr;
    bo->obj_addr    = b.obj_addr;
    return 0;
}

int rocket_bo_alloc32(int fd, size_t size, rocket_bo *bo)
{
    int rc = rocket_bo_alloc(fd, size, bo);
    if (rc != 0) return rc;
    /* The LAST byte is what has to be encodable, not the first — and on this driver
     * that is the common case rather than an edge one: the vendor allocator works
     * TOP-DOWN from the 4 GiB ceiling, so a BO whose last byte is exactly 0xFFFFFFFF
     * is what a first allocation looks like. Testing base+size instead of
     * base+size-1 refuses it. */
    if (size != 0 && ((bo->dma_address + size - 1) >> 32) != 0) {
        RKNPU_LOGE("rocket_bo_alloc32(%zu): IOVA 0x%llx..0x%llx leaves the low 4 GB the "
                   "regcmd's 32-bit address fields can encode\n", size,
                   (unsigned long long)bo->dma_address,
                   (unsigned long long)(bo->dma_address + size - 1));
        rocket_bo_free(fd, bo);
        return ROCKET_E_DEVICE;
    }
    return 0;
}

int rocket_bo_ensure32(int fd, rocket_bo *bo, size_t need)
{
    if (bo->handle && bo->size >= need)
        return 0;                     /* reuse: contents, including padding, intact */
    rocket_bo_free(fd, bo);
    int rc = rocket_bo_alloc32(fd, need, bo);
    return rc == 0 ? 1 : rc;          /* 1 = REALLOCATED, so any zero padding is gone */
}

void rocket_bo_free(int fd, rocket_bo *bo)
{
    struct rknpu_dev *d = dev_find(fd);
    if (!bo->handle && !bo->ptr) return;
    if (!d) { RKNPU_LOGE("rocket_bo_free on unknown fd %d — leaking handle %u\n",
                         fd, bo->handle); return; }
    struct rknpu_bo_priv b = { .handle = bo->handle, .obj_addr = bo->obj_addr,
                               .dma_addr = bo->dma_address,
                               .size = page_up(bo->size ? bo->size : 1), .ptr = bo->ptr };
    bo_destroy(d, &b);
    bo->handle = 0; bo->ptr = NULL; bo->obj_addr = 0; bo->dma_address = 0; bo->size = 0;
}

/* ============================================================================
 * SECTION — Cache maintenance
 *
 * The mainline PREP_BO does two jobs at once: it waits on the BO's write fence AND
 * syncs for the CPU. Here the submit already blocked until the job retired, so only
 * the sync remains — `timeout_ns` has nothing left to wait for and is accepted and
 * ignored rather than refused, because every caller above the seam passes it.
 * `dir` documents caller intent on the mainline provider too and is likewise unused.
 * ==========================================================================*/

int rocket_bo_prep(int fd, rocket_bo *bo, int dir, uint64_t timeout_ns)
{
    struct rknpu_dev *d = dev_find(fd);
    (void)dir; (void)timeout_ns;
    if (!d) return -EBADF;
    return bo_sync(d, bo->obj_addr, 0, page_up(bo->size ? bo->size : 1),
                   RKNPU_MEM_SYNC_FROM_DEVICE);
}

int rocket_bo_fini(int fd, rocket_bo *bo)
{
    struct rknpu_dev *d = dev_find(fd);
    if (!d) return -EBADF;
    return bo_sync(d, bo->obj_addr, 0, page_up(bo->size ? bo->size : 1),
                   RKNPU_MEM_SYNC_TO_DEVICE);
}

/* MEM_SYNC takes an offset and a size, so a range maps straight onto one ioctl. The
 * seam's contract — ranges ASCENDING, non-overlapping, inside the BO, and n==0
 * meaning the whole object — is enforced here rather than by the driver, which checks
 * none of the three: an out-of-range sync on this uAPI is a silent no-op on memory
 * that then stays stale. FINI's ranges must cover everything the CPU dirtied. */
static int sync_ranges(int fd, rocket_bo *bo, const rocket_bo_range *r, unsigned n,
                       uint32_t dir)
{
    struct rknpu_dev *d = dev_find(fd);
    if (!d) return -EBADF;
    if (n == 0)
        return bo_sync(d, bo->obj_addr, 0, page_up(bo->size ? bo->size : 1), dir);

    uint64_t prev_end = 0;
    for (unsigned i = 0; i < n; i++) {
        if (r[i].offset < prev_end || r[i].size == 0 ||
            r[i].offset + r[i].size > bo->size ||
            r[i].offset + r[i].size < r[i].offset) {
            RKNPU_LOGE("bad range %u/%u (offset=%llu size=%llu, BO=%zu): ranges must be "
                       "ascending, non-overlapping and inside the object\n", i, n,
                       (unsigned long long)r[i].offset, (unsigned long long)r[i].size,
                       bo->size);
            return -EINVAL;
        }
        prev_end = r[i].offset + r[i].size;
    }
    for (unsigned i = 0; i < n; i++) {
        int rc = bo_sync(d, bo->obj_addr, r[i].offset, r[i].size, dir);
        if (rc < 0) return rc;
    }
    return 0;
}

int rocket_bo_prep_ranges(int fd, rocket_bo *bo, const rocket_bo_range *r, unsigned n,
                          uint64_t timeout_ns)
{
    (void)timeout_ns;   /* the submit already blocked; nothing left to wait for */
    return sync_ranges(fd, bo, r, n, RKNPU_MEM_SYNC_FROM_DEVICE);
}

int rocket_bo_fini_ranges(int fd, rocket_bo *bo, const rocket_bo_range *r, unsigned n)
{
    return sync_ranges(fd, bo, r, n, RKNPU_MEM_SYNC_TO_DEVICE);
}

/* One ioctl per range, natively — unlike the mainline path, where a kernel older than
 * interface 1.5 falls back to the whole object. So the saving is always available. */
int rocket_bo_ranges_supported(void) { return 1; }

/* ============================================================================
 * SECTION — Capabilities
 *
 * These gate CALLER behaviour, so each answers about what this provider actually
 * does, not about what the hardware could be made to do.
 * ==========================================================================*/

/* Chained (self-linking) regcmd layout. Reported ON, because on this uAPI it is the
 * ONLY multi-program mode there is.
 *
 * rknpu_job_commit() programs PC_DATA_ADDR from first_task->regcmd_addr and
 * PC_DATA_AMOUNT from first_task->regcfg_amount, writes task_number into
 * PC_TASK_CONTROL, and kicks once. The regcmd_addr of tasks 1..n-1 is NEVER READ.
 * So "n tasks" here means ONE contiguous register-command stream containing n
 * programs whose own trailers link to the next — which is exactly the chained layout
 * rocket_chain.c builds, and exactly what our patches/rocket 086 adds to the mainline
 * driver. The BSP driver has done it natively all along, and has no un-chained
 * multi-program mode at all. [source-confirmed: rknpu_job.c 0.9.8]
 *
 * The consequence for an UNCHAINED multi-task submit is a hang, not an error: the
 * hardware runs the first program, its task counter stops at 1, and the job waits out
 * its timeout. Measured — "task counter: 1 ... require mask: 0x300" in dmesg after a
 * 16-tile matmul. So the two cases are split apart in submit_tasks() rather than both
 * being handed to one ioctl. */
int rocket_batched_submit_supported(void) { return 1; }

/* The completion is the single interrupt PC_TASK_CONTROL's task_number gates: it
 * fires when the hardware task counter reaches n, so the wait covers the WHOLE kick
 * rather than starting at the first program. That is what this query gates a refusal
 * on, and here it holds. The chain length is bounded instead by the 12-bit task-number
 * field; the driver chunks anything longer than max_submit_number (4095 on RK3588)
 * into successive kicks itself. [source-confirmed] */
int rocket_batch_completion_tracked(void) { return 1; }

/* CHAINING IS THE NATIVE MODE HERE, so it is the default rather than an opt-in.
 *
 * A job programs only the first task's address, so a multi-program job can ONLY be a
 * contiguous self-linked chain — the gapped per-task layout is not a second option
 * here, it is n separate jobs. Chaining also keeps a batch to ONE job, which is what
 * CBUF reuse needs (see rocket_submit_batch_atomic), so the two questions have the same
 * answer. Measured at 512x3840x4096 fp16, three warm rounds: 36.8-39.3 ms chained
 * against 68-77 ms unchained on a pinned core, and 26.1-27.7 ms chained on the driver's
 * own core scheduler. ROCKET_BATCH_SUBMIT=0 still forces the gapped path. */
int rocket_batched_submit_native(void) { return 1; }

/* ONE SUBMIT IS NOT ONE JOB HERE — unless the caller chained it.
 *
 * This uAPI's job carries ONE program address: rknpu_job_commit() programs
 * PC_DATA_ADDR from first_task->regcmd_addr and never reads tasks 1..n-1's. So n
 * UNCHAINED programs can only be n submits, which is n independent jobs, and the
 * driver is free to run another context's job between any two of them (it takes the
 * next entry off subcore_data->todo_list the moment the previous one retires).
 * Mainline instead holds core->in_flight_job across the whole task sequence.
 *
 * What that costs is CBUF operand reuse, and it costs it silently: a tile whose CNA
 * WEIGHT_REUSE/DATA_REUSE bit is set reads the operand the interleaving job left
 * behind and computes a full, correctly sized, plausible surface. Measured on one
 * 128x1024x1024 fp16 matmul: 29 of 120 runs corrupt with reuse on, 0 of 120 with it
 * off, 0 of 80 with the chained layout — and three independent single-threaded
 * PROCESSES corrupt each other, which is what puts it below the library.
 *
 * A CHAINED submit is one kick and does not have the problem, so the caller's own
 * chained flag is the other half of the gate; see rocket_batched_submit_supported().
 * ROCKET_BATCH_SUBMIT=1 is therefore both the faster and the reuse-preserving setting
 * on this provider, and it needs no kernel patch here. */
int rocket_submit_batch_atomic(void) { return 0; }

/* The driver writes last_task->int_mask into INT_MASK, so the completing block is
 * whatever the job's LAST program raises — which is the same thing the mainline flag
 * says, expressed as a mask instead of a bit. A pooling program completes on the PPU
 * pair (0xc00) rather than the DPU pair (0x300), and without the flag it would wait
 * for a DPU completion that a pool never raises.
 *
 * Reported ON because the translation is direct, NOT because it has been measured: a
 * wrong mask here costs the job its timeout, which is loud, rather than computing a
 * wrong surface. The pooling gates are the measurement. */
int rocket_ppu_done_supported(void) { return 1; }

/* NO_DPU_DONE names a completion class for a driver that POLLS for one and wants to be
 * told it will not arrive. This driver does not poll: it is handed INT_MASK per task and
 * waits on that block's interrupt, so the class is already named by the mask
 * rocket_submit_tasks_flags() derives, and the hint has nothing left to say.
 *
 * Reported OFF rather than ON-because-harmless. The provider does ignore the bit -- this
 * uAPI has no flags word to reject it with, unlike mainline -- but the probe answers
 * whether the running kernel HONORS the flag, and this one does not. Answering 1 would
 * put a hint on the wire that buys nothing and would read, to anyone tracing a job, as a
 * completion class the driver had agreed to. */
int rocket_no_dpu_done_supported(void) { return 0; }

/* ============================================================================
 * SECTION — Submit
 * ==========================================================================*/

/* Which core a job is submitted to. DEFAULT: the driver's AUTO scheduler, which picks
 * the least-loaded core per job (rknpu_schedule_core_index) and rewrites the mask.
 * RKNPU_CORE_MASK overrides, as a decimal or 0x-prefixed value: 0 = AUTO, 1/2/4 = core
 * 0/1/2 explicitly. Read once.
 *
 * Pinning every job to core 0 is what this started as, and it left the library's
 * multi-core fan-out with nothing to fan out to: 512x3840x4096 fp16 measured 68-77 ms
 * pinned against 47-48 ms on AUTO, and 26.1-27.7 ms with AUTO plus the chained layout —
 * against 23.5-24.4 ms for the same shape through mainline rocket. AUTO does NOT fix
 * the CBUF-reuse hazard (4 of 8 runs still corrupt with reuse forced on), because that
 * is about any interleaving job rather than about which core runs it.
 *
 * SINGLE-CORE VALUES ONLY. A ganged mask (3/5/6/7) makes the driver commit the SAME
 * task range to each named core, so every core would run the whole program and write
 * the same output — that is the vendor runtime's multi-core-model split, which needs
 * a per-core task range this provider does not build. Anything else falls back to
 * core 0 rather than silently running the program twice. */
static uint32_t rknpu_core_mask_env(void)
{
    static _Atomic int cached = -1;
    int v = cached;
    if (v < 0) {
        const char *e = getenv("RKNPU_CORE_MASK");
        v = e ? (int)strtoul(e, NULL, 0) : (int)RKNPU_CORE_AUTO_MASK;
        if (v != (int)RKNPU_CORE_AUTO_MASK && v != (int)RKNPU_CORE0_MASK &&
            v != (int)RKNPU_CORE1_MASK && v != (int)RKNPU_CORE2_MASK) {
            RKNPU_LOGE("RKNPU_CORE_MASK=%s is not a single core (0 auto, 1/2/4); "
                       "using AUTO\n", e ? e : "");
            v = (int)RKNPU_CORE_AUTO_MASK;
        }
        cached = v;
    }
    return (uint32_t)v;
}

/* Grow the per-fd task BO to hold at least `n` rknpu_task entries.
 *
 * KERNEL_MAPPING is not optional: the driver reads the array through
 * task_obj->kv_addr, which is populated only for that flag. Without it the pointer is
 * NULL and the driver dereferences it — a crash, not a refusal. */
static int taskbo_ensure(struct rknpu_dev *d, uint32_t n)
{
    if (d->taskbo.handle && d->taskbo_cap >= n) return 0;
    bo_destroy(d, &d->taskbo);
    d->taskbo_cap = 0;
    uint32_t want = n < 64 ? 64 : n;
    int rc = bo_create(d, (size_t)want * sizeof(struct rknpu_task),
                       RKNPU_MEM_KERNEL_MAPPING, &d->taskbo);
    if (rc < 0) return rc;
    d->taskbo_cap = (uint32_t)(d->taskbo.size / sizeof(struct rknpu_task));
    return 0;
}

/* ONE ioctl carrying `n_prog` programs starting at tasks[0].
 *
 * n_prog > 1 is only correct when the programs are CHAINED — see
 * rocket_batched_submit_supported(). The task array still gets n_prog entries even
 * though only the first one's address and amount are read, because the driver takes
 * INT_MASK from task_base[task_start + task_number - 1]: a short array would leave
 * that entry zero, INT_MASK would be 0, and the job would never complete. */
/* Fill the task array, sync it, submit. The caller holds d->submit_lock: everything
 * from taskbo_ensure() to the ioctl touches one buffer the device owns for the length
 * of the job. */
static int submit_locked(struct rknpu_dev *d, const rocket_task_desc *tasks,
                         uint32_t n_prog, uint32_t int_mask, uint32_t timeout_ms)
{
    int rc = taskbo_ensure(d, n_prog);
    if (rc < 0) return rc;

    struct rknpu_task *t = d->taskbo.ptr;
    for (uint32_t i = 0; i < n_prog; i++) {
        /* regcfg_amount is regcmd_count minus the 4-op control trailer this uAPI adds
         * back itself. A count below that trailer is not a program. */
        if (tasks[i].regcmd_count <= RKNPU_REGCFG_EXTRA) {
            RKNPU_LOGE("task %u/%u: regcmd_count=%u does not even cover the %u-op "
                       "control trailer\n", i, n_prog, tasks[i].regcmd_count,
                       RKNPU_REGCFG_EXTRA);
            return -EINVAL;
        }
        memset(&t[i], 0, sizeof t[i]);
        t[i].int_mask      = int_mask;
        t[i].regcfg_amount = tasks[i].regcmd_count - RKNPU_REGCFG_EXTRA;
        t[i].regcmd_addr   = tasks[i].regcmd;
        /* enable_mask / int_clear / op_idx / regcfg_offset are never read by the
         * driver; int_status is written BACK. All left zero by the memset. */
    }
    rc = bo_sync(d, d->taskbo.obj_addr, 0,
                 (uint64_t)n_prog * sizeof(struct rknpu_task), RKNPU_MEM_SYNC_TO_DEVICE);
    if (rc < 0) return rc;

    struct rknpu_submit s;
    memset(&s, 0, sizeof s);
    s.flags           = RKNPU_JOB_PC | RKNPU_JOB_BLOCK;
    s.timeout         = timeout_ms ? timeout_ms : 2000;
    s.task_start      = 0;
    s.task_number     = n_prog;
    s.priority        = 0;
    s.task_obj_addr   = d->taskbo.obj_addr;   /* the kernel object, not the handle */
    s.iommu_domain_id = 0;
    /* PC_DMA_BASE_ADDR. Mainline rocket writes 0 here and programs the full IOVA into
     * PC_BASE_ADDRESS, which is exactly what regcmd_addr already is, so 0 is the
     * equivalence-preserving choice and is what was measured working. hello2.c sets
     * this to the regcmd address instead; that file is an RE scratch pad with no
     * evidence it ever completed. */
    s.task_base_addr  = 0;
    s.core_mask       = rknpu_core_mask_env();
    s.fence_fd        = -1;
    /* Every entry, not just [0]: with the AUTO mask the driver picks the core and
     * then reads subcore_task[core_index], so a job that lands on core 1 or 2 would
     * otherwise find task_number = 0 and never complete. Harmless for a fixed mask. */
    for (int c = 0; c < 3; c++) {
        s.subcore_task[c].task_start  = 0;
        s.subcore_task[c].task_number = n_prog;
    }

    if (ioctl(d->fd, ioc(d, RKNPU_SUBMIT_NR, sizeof s), &s) < 0) {
        int e = errno;
        RKNPU_LOGE("SUBMIT(%u programs, int_mask=0x%x): %s — dmesg carries the irq "
                   "status the handler saw against the required mask, and the hardware "
                   "task counter it stopped at\n", n_prog, int_mask, strerror(e));
        return -e;
    }
    d->last_hw_elapse_ns = s.hw_elapse_time;
    count_submit(n_prog, s.hw_elapse_time);
    return 0;
}

static int submit_kick(struct rknpu_dev *d, const rocket_task_desc *tasks,
                       uint32_t n_prog, uint32_t int_mask, uint32_t timeout_ms)
{
    pthread_mutex_lock(&d->submit_lock);
    int rc = submit_locked(d, tasks, n_prog, int_mask, timeout_ms);
    pthread_mutex_unlock(&d->submit_lock);
    return rc;
}

static int submit_tasks(int fd, const rocket_task_desc *tasks, uint32_t n_tasks,
                        uint32_t job_flags, uint32_t timeout_ms)
{
    struct rknpu_dev *d = dev_find(fd);
    if (!d) return -EBADF;
    if (n_tasks == 0) return 0;

    /* Which block the job's last program completes on. The driver takes INT_MASK from
     * the last task, so this is per-job by construction here. */
    uint32_t int_mask = (job_flags & ROCKET_JOB_PPU_DONE) ? RKNPU_TASK_INT_MASK_PPU
                                                          : RKNPU_TASK_INT_MASK_DPU;

    /* CHAINED: one kick, the hardware's task counter walks the stream. */
    if (job_flags & ROCKET_JOB_BATCHED)
        return submit_kick(d, tasks, n_tasks, int_mask, timeout_ms);

    /* UNCHAINED: n independent programs at n unrelated addresses, which this uAPI
     * cannot express in one job — only the first program's address is ever
     * programmed. So they go one per submit. Correct (this is the single-task shape
     * M2 measured bit-exact), and explicitly n ioctls rather than 1: the submit
     * counters are what tell the two apart. */
    for (uint32_t i = 0; i < n_tasks; i++) {
        int rc = submit_kick(d, &tasks[i], 1, int_mask, timeout_ms);
        if (rc < 0) return rc;
    }
    return 0;
}

int rocket_submit_tasks(int fd, const rocket_task_desc *tasks, uint32_t n_tasks,
                        const uint32_t *in_handles, uint32_t n_in,
                        const uint32_t *out_handles, uint32_t n_out)
{
    /* No residency list on this uAPI: the IOMMU domain maps every object this fd owns
     * for the device's whole lifetime, so the handles have nothing to declare to. */
    (void)in_handles; (void)n_in; (void)out_handles; (void)n_out;
    return submit_tasks(fd, tasks, n_tasks, 0, 0);
}

int rocket_submit_tasks_flags(int fd, const rocket_task_desc *tasks, uint32_t n_tasks,
                              const uint32_t *in_handles, uint32_t n_in,
                              const uint32_t *out_handles, uint32_t n_out,
                              uint32_t job_flags)
{
    (void)in_handles; (void)n_in; (void)out_handles; (void)n_out;
    /* ROCKET_JOB_NO_DPU_DONE is advisory even on mainline — a completion that arrives
     * retires the job whatever the hint says — so it is ignorable here. BATCHED and
     * PPU_DONE both change what this provider does; see submit_tasks(). */
    return submit_tasks(fd, tasks, n_tasks, job_flags, 0);
}

size_t rocket_submit_scratch_size(uint32_t max_tasks)
{
    /* This provider cannot use caller-owned HOST memory for the task array — the
     * driver reads it through the object's kernel mapping — so it keeps its own
     * device-side BO and grows it on demand. The seam's scratch is left unused, and
     * reporting 0 stops a caller from allocating a buffer nobody reads. The
     * no-alloc property callers want from _pre() still holds: the task BO is
     * allocated once per fd, not once per submit. */
    (void)max_tasks;
    return 0;
}

int rocket_submit_tasks_pre(int fd, void *scratch, const rocket_task_desc *tasks,
                            uint32_t n_tasks, const uint32_t *in_handles, uint32_t n_in,
                            const uint32_t *out_handles, uint32_t n_out,
                            uint32_t job_flags)
{
    (void)scratch;
    return rocket_submit_tasks_flags(fd, tasks, n_tasks, in_handles, n_in,
                                     out_handles, n_out, job_flags);
}

int rocket_submit_matmul_flags(int fd, const rocket_bo *regcmd_bo, uint32_t regcmd_count,
                               const uint32_t *in_handles, uint32_t n_in,
                               const uint32_t *out_handles, uint32_t n_out,
                               uint32_t job_flags)
{
    rocket_task_desc t = { .regcmd = (uint32_t)regcmd_bo->dma_address,
                           .regcmd_count = regcmd_count };
    return rocket_submit_tasks_flags(fd, &t, 1, in_handles, n_in, out_handles, n_out,
                                     job_flags);
}

int rocket_submit_matmul(int fd, const rocket_bo *regcmd_bo, uint32_t regcmd_count,
                         const uint32_t *in_handles, uint32_t n_in,
                         const uint32_t *out_handles, uint32_t n_out,
                         uint32_t timeout_ms)
{
    rocket_task_desc t = { .regcmd = (uint32_t)regcmd_bo->dma_address,
                           .regcmd_count = regcmd_count };
    (void)in_handles; (void)n_in; (void)out_handles; (void)n_out;
    return submit_tasks(fd, &t, 1, 0, timeout_ms);
}

int rocket_submit_jobs(int fd, const rocket_job_desc *jobs, uint32_t n_jobs)
{
    /* Mainline puts N jobs in ONE ioctl so the kernel can spread them across cores.
     * This uAPI's multicore control is core_mask plus a per-core subcore_task[] range
     * WITHIN one job, which is a different shape, and a blocking submit forecloses the
     * overlap anyway. So this runs them in order: correct, and explicitly not
     * concurrent. The submit counters tell the two apart — N ioctls here against the
     * mainline provider's 1. */
    for (uint32_t i = 0; i < n_jobs; i++) {
        int rc = submit_tasks(fd, jobs[i].tasks, jobs[i].n_tasks, 0, 0);
        if (rc < 0) return rc;
    }
    return 0;
}

/* ============================================================================
 * SECTION — instruments this uAPI has and the mainline one does not
 * ==========================================================================*/

int64_t rknpu_last_hw_elapse_ns(int fd)
{
    struct rknpu_dev *d = dev_find(fd);
    return d ? d->last_hw_elapse_ns : -1;
}

uint64_t rknpu_hw_elapse_total_ns(uint64_t *n_submits)
{
    if (n_submits)
        *n_submits = atomic_load_explicit(&g_hw_submits, memory_order_relaxed);
    return atomic_load_explicit(&g_hw_ns, memory_order_relaxed);
}

int rknpu_action(int fd, uint32_t action, uint32_t *value)
{
    struct rknpu_dev *d = dev_find(fd);
    struct rknpu_action a;
    if (!d || !value) return -EINVAL;
    memset(&a, 0, sizeof a);
    a.flags = action;
    a.value = *value;
    if (ioctl(d->fd, ioc(d, RKNPU_ACTION_NR, sizeof a), &a) < 0)
        return -errno;
    *value = a.value;
    return 0;
}
