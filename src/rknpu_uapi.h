// SPDX-License-Identifier: MIT
// Copyright (C) 2026 The rknpu-submit authors
/*
 * rknpu_uapi.h — the part of the `rknpu` uAPI this provider uses, in the provider's own
 * terms.
 *
 * This is not the driver's header. The names, the grouping and the comments are this
 * project's, and only what rknpu_provider.c calls is declared. The binary contract is
 * not ours to choose: each structure's size and field offsets, each request word and
 * each flag value match what the driver compiles to on aarch64 and x86-64. Every
 * structure is followed by _Static_asserts on its size and on the offset of every field
 * (the DRM query's on LP64 only), so a layout slip fails the build instead of reaching
 * the kernel as a plausible ioctl. A size check alone would not do: the 0.9.6 change
 * described in section 5 kept one structure's size and moved a meaning.
 *
 * Why the provider states the interface itself rather than including it: a BSP kernel
 * does not install the driver's header, and a provider that builds only where the BSP
 * kernel's source tree happens to be unpacked is not one anyone can build.
 *
 * Written against driver version 0.9.8 / 20240828, which is what `[drm] Initialized
 * rknpu 0.9.8 20240828` reports on the boards the provider was validated on. The `rknpu`
 * driver versions independently of the kernel it ships in, so read its version
 * (BSP_CTL_DRIVER_VERSION) before relying on any layout here; section 5 says which hold
 * where.
 *
 * Rockchip also publishes this interface under Apache-2.0, in its RT-Thread BSP as
 * bsp/rockchip/common/drivers/rknpu/rknpu_ioctl.h (Copyright 2024 Rockchip Electronics
 * Co., Ltd). Its task, buffer and action structures have the layouts below. Its submit
 * structure keeps the register-config word that 0.9.6 turned into `domain`, and its
 * allocation structure ends before the two words 0.9.6 appended (section 5).
 *
 * Sections, in the order the provider meets them:
 *   1. The node        the DRM identity query, and how a request word is formed
 *   2. Memory          allocate, mmap offset, cache maintenance, free
 *   3. Submit          the task array, the completion masks, the job
 *   4. Control         the get/set multiplexer, used here for the driver version
 *   5. Versions        which driver releases these layouts hold for
 *
 * Each command's heading gives its name in the driver (MEM_CREATE, SUBMIT, ...), which
 * is also what the provider's log lines print, so a call can be followed into the
 * driver.
 */
#ifndef RKNPU_UAPI_H
#define RKNPU_UAPI_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

/* Pins one field's byte offset. Used after each structure below. */
#define BSP_AT(tag, field, off) \
    _Static_assert(offsetof(struct tag, field) == (off), #tag "." #field " is at " #off)

/* ===========================================================================
 * 1. The node
 * ======================================================================== */

/* In its render-node build the `rknpu` driver registers as a DRM driver, so its node
 * answers the DRM core's own ioctls, and its commands occupy the range DRM reserves for
 * a driver. (The misc-node build is refused at open; see rknpu_provider.c.) Every
 * command below is reached with the request word
 *
 *     _IOWR(BSP_DRM_IOC_TYPE, BSP_DRM_DRIVER_BASE + BSP_CMD_x, struct <its argument>)
 *
 * which encodes the argument's size as well as the command; ioc() in rknpu_provider.c
 * forms it. Section 5 says what the kernel does when that size and the driver's differ.
 */
#define BSP_DRM_IOC_TYPE    'd'
#define BSP_DRM_DRIVER_BASE 0x40

/* The DRM core's version query, which the provider uses for the driver NAME only: it is
 * how the NPU's render node is told apart from the display's, whose numbering varies by
 * board.
 *
 * Each string travels with a length word. The caller sets the word to its buffer's
 * capacity; the kernel copies at most that many bytes, adds no terminator, and then
 * replaces the word with the string's full length, which can exceed the capacity
 * [source-confirmed: drm_ioctl.c, drm_copy_field()]. The integer and pointer widths are
 * the kernel's own `int`, `size_t` and pointer, so the layout follows the process ABI
 * and is pinned only for LP64. */
struct bsp_node_version {
    int     major;          /* out */
    int     minor;          /* out */
    int     patch;          /* out */
    size_t  name_bytes;     /* in: capacity of `name`; out: length of the driver name */
    char   *name;
    size_t  date_bytes;     /* the same contract, for the release date */
    char   *date;
    size_t  desc_bytes;     /* and for the description */
    char   *desc;
};
#define BSP_IOC_NODE_VERSION _IOWR(BSP_DRM_IOC_TYPE, 0x00, struct bsp_node_version)

#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(struct bsp_node_version) == 64, "bsp_node_version size (LP64)");
BSP_AT(bsp_node_version, major, 0);       BSP_AT(bsp_node_version, minor, 4);
BSP_AT(bsp_node_version, patch, 8);       BSP_AT(bsp_node_version, name_bytes, 16);
BSP_AT(bsp_node_version, name, 24);       BSP_AT(bsp_node_version, date_bytes, 32);
BSP_AT(bsp_node_version, date, 40);       BSP_AT(bsp_node_version, desc_bytes, 48);
BSP_AT(bsp_node_version, desc, 56);
#endif

/* ===========================================================================
 * 2. Memory
 *
 * A buffer has two names. The GEM handle is what MEM_MAP and MEM_DESTROY look the buffer
 * up by. The object cookie is the address of the kernel's object for the buffer, handed
 * out by MEM_CREATE and handed back, as an opaque 64-bit value, to MEM_SYNC and (for the
 * task array) to SUBMIT. Keep it 64-bit everywhere: it is a kernel pointer.
 * ======================================================================== */

/* ---- Allocate (MEM_CREATE) ---------------------------------------------- */
#define BSP_CMD_ALLOC 0x02

struct bsp_alloc {
    uint32_t gem_handle;    /* out */
    uint32_t flags;         /* in: BSP_ALLOC_* */
    uint64_t bytes;         /* in: requested; out: what the driver allocated */
    uint64_t obj_cookie;    /* out */
    uint64_t iova;          /* out: where the NPU sees the buffer */
    uint64_t sram_bytes;    /* in/out: on-chip SRAM wanted and granted; 0 here */
    int32_t  domain;        /* in: IOMMU domain to map into; 0 here */
    uint32_t cores;         /* in: a core mask in 0.9.8; 0 here */
};
_Static_assert(sizeof(struct bsp_alloc) == 48, "bsp_alloc size");
BSP_AT(bsp_alloc, gem_handle, 0);   BSP_AT(bsp_alloc, flags, 4);
BSP_AT(bsp_alloc, bytes, 8);        BSP_AT(bsp_alloc, obj_cookie, 16);
BSP_AT(bsp_alloc, iova, 24);        BSP_AT(bsp_alloc, sram_bytes, 32);
BSP_AT(bsp_alloc, domain, 40);      BSP_AT(bsp_alloc, cores, 44);

/* bsp_alloc.flags. The driver defines more bits than these; the provider sets no
 * others. */

/* Backed by individual pages rather than one physically contiguous block.
 *
 * A contiguous buffer is the absence of this bit, and asking for one is a trap on this
 * driver. rknpu_gem_alloc_buf() routes a scattered request to rknpu_gem_get_pages(),
 * which populates the object's page array. A contiguous request goes to
 * dma_alloc_attrs() with FORCE_CONTIGUOUS instead, and when CMA cannot satisfy it the
 * driver falls back by marking the object scattered and re-allocating, WITHOUT going
 * back for the pages. mmap then sees the mark, takes rknpu_gem_mmap_pages(), and
 * __vm_map_pages() refuses against an unpopulated array: 'failed to map pages into vma:
 * -6'. So a contiguous allocation stops being mappable the moment the CMA pool runs
 * short: 256 MiB on the board this was found on, which a resident-weight workload passes
 * without trying.
 *
 * Asking for scattered pages up front avoids both the CMA pressure and the fallback.
 * There is no cost: every buffer goes through the IOMMU, so the device sees one
 * contiguous IOVA range either way. [HW sweep + source-confirmed] */
#define BSP_ALLOC_SCATTER     (1u << 0)

/* The process's mapping is cacheable rather than uncached. MEM_SYNC refuses a buffer
 * without this bit, and the provider syncs around every device access. */
#define BSP_ALLOC_CACHED      (1u << 1)

/* The driver keeps a kernel-side mapping as well. Required for the task array, which the
 * driver reads through that mapping (section 3). */
#define BSP_ALLOC_KMAP        (1u << 3)

/* The 0.9.8 driver does not test this bit: a buffer is IOMMU-mapped because the device's
 * IOMMU is enabled, whatever the bit says [source-confirmed: rknpu 0.9.8]. Setting it
 * costs nothing. */
#define BSP_ALLOC_VIA_IOMMU   (1u << 4)

/* The driver allocates the pages zeroed. See bo_create() in rknpu_provider.c for what
 * that does and does not guarantee to the process's mapping under
 * BSP_ALLOC_IOVA_TIGHT. */
#define BSP_ALLOC_ZERO        (1u << 5)

/* Without this bit the driver maps a buffer with the kernel's generic
 * iommu_dma_map_sg(), whose IOVA allocator is alloc_iova_fast(size_aligned=true) ->
 * alloc_iova(), which does size = __roundup_pow_of_two(size) and then aligns the result
 * to that same boundary. So a buffer one page over a power of two costs the NEXT power
 * of two of IOVA and must start on it. Our buffers are sized M*N*2 plus tile padding,
 * which lands just over a power of two as the common case, so the waste is close to 2x
 * and the alignment fragments what is left.
 *
 * Setting it takes the driver's own rknpu_iommu_dma_map_sg(), which allocates with
 * size_aligned=false: no roundup, no alignment. Only from 0.9.7; see section 5.
 * [HW sweep + source-confirmed] */
#define BSP_ALLOC_IOVA_TIGHT  (1u << 10)

/* ---- mmap offset (MEM_MAP) ------------------------------------------------ */
/* Turns a handle into the offset to pass to mmap() on the same fd. */
#define BSP_CMD_MMAP_OFFSET 0x03

struct bsp_mmap_offset {
    uint32_t gem_handle;    /* in */
    uint32_t pad;
    uint64_t mmap_offset;   /* out */
};
_Static_assert(sizeof(struct bsp_mmap_offset) == 16, "bsp_mmap_offset size");
BSP_AT(bsp_mmap_offset, gem_handle, 0);   BSP_AT(bsp_mmap_offset, pad, 4);
BSP_AT(bsp_mmap_offset, mmap_offset, 8);

/* ---- Cache maintenance (MEM_SYNC) ---------------------------------------- */
#define BSP_CMD_SYNC 0x05

struct bsp_sync {
    uint32_t direction;     /* in: one BSP_SYNC_FOR_* */
    uint32_t pad;
    uint64_t obj_cookie;    /* in */
    uint64_t start;         /* in: byte offset into the buffer */
    uint64_t bytes;         /* in */
};
_Static_assert(sizeof(struct bsp_sync) == 32, "bsp_sync size");
BSP_AT(bsp_sync, direction, 0);     BSP_AT(bsp_sync, pad, 4);
BSP_AT(bsp_sync, obj_cookie, 8);    BSP_AT(bsp_sync, start, 16);
BSP_AT(bsp_sync, bytes, 24);

#define BSP_SYNC_FOR_DEVICE (1u << 0)   /* CPU writes become visible to the NPU */
#define BSP_SYNC_FOR_CPU    (1u << 1)   /* NPU writes become visible to the CPU */

/* ---- Free (MEM_DESTROY) --------------------------------------------------- */
#define BSP_CMD_FREE 0x04

struct bsp_free {
    uint32_t gem_handle;    /* in: the lookup key on the render-node build */
    uint32_t pad;
    uint64_t obj_cookie;    /* in */
};
_Static_assert(sizeof(struct bsp_free) == 16, "bsp_free size");
BSP_AT(bsp_free, gem_handle, 0);    BSP_AT(bsp_free, pad, 4);
BSP_AT(bsp_free, obj_cookie, 8);

/* ===========================================================================
 * 3. Submit
 *
 * A job is a range of entries in a task array, which lives in a buffer allocated with
 * BSP_ALLOC_KMAP, and one SUBMIT naming that buffer by its object cookie.
 * ======================================================================== */

/* ---- The task array entry -----------------------------------------------
 *
 * Packed, as the driver declares it, so the structure's alignment is 1. What each field
 * does on this path, decoded from rknpu_job.c (0.9.8) rather than guessed:
 *
 *   program_iova   where the register program is.
 *   body_ops       how long it is, less its trailer. See BSP_TRAILER_OPS.
 *   irq_wait       the only other live input. See BSP_IRQ_DPU_PAIR.
 *   irq_status     written BACK by the driver into the job's last entry: an OUTPUT, not
 *                  an input.
 *   unused_enable  never read by the driver.
 *   unused_clear   never read. The driver writes the first entry's irq_wait to the
 *                  INT_CLEAR register, not this field.
 *   op_index       unused on this path.
 *   unused_offset  never read by the driver.
 *
 * Beyond the program, none of these has a counterpart in the mainline `rocket` task. The
 * provider zeroes each entry before filling it, so `mode` and the unused fields go down
 * as 0. */
struct bsp_task {
    uint32_t mode;           /* 0 */
    uint32_t op_index;
    uint32_t unused_enable;
    uint32_t irq_wait;       /* in: BSP_IRQ_*_PAIR */
    uint32_t unused_clear;
    uint32_t irq_status;     /* out */
    uint32_t body_ops;       /* in: regcmd entries less BSP_TRAILER_OPS */
    uint32_t unused_offset;
    uint64_t program_iova;   /* in: the register program's device address */
} __attribute__((packed));
_Static_assert(sizeof(struct bsp_task) == 40, "bsp_task size");
BSP_AT(bsp_task, mode, 0);            BSP_AT(bsp_task, op_index, 4);
BSP_AT(bsp_task, unused_enable, 8);   BSP_AT(bsp_task, irq_wait, 12);
BSP_AT(bsp_task, unused_clear, 16);   BSP_AT(bsp_task, irq_status, 20);
BSP_AT(bsp_task, body_ops, 24);       BSP_AT(bsp_task, unused_offset, 28);
BSP_AT(bsp_task, program_iova, 32);

/*
 * irq_wait reaches the INT_MASK register, and the IRQ handler completes the job only
 * when rknpu_fuzz_status(status) == the mask. fuzz_status collapses the status word into
 * block PAIRS (0x3 CNA_FEATURE, 0xc CNA_WEIGHT, 0x30 CNA_CSC, 0xc0 CORE, 0x300 DPU,
 * 0xc00 PPU), so the mask names which blocks must have signalled.
 *
 * Our programs terminate in a DPU write, and the mainline `rocket` driver hard-codes
 * exactly PC_INTERRUPT_MASK_DPU_0|DPU_1 == 0x300 for the same programs; hello2.c's
 * IRQ_DPU_GROUP0/1 agree. Three independent sources, so this is not a guess.
 *
 * THE PAIR IS REQUIRED, not a single bit. The measured irq_status came back 0x100
 * (DPU_0) at M=4 K=32 N=16 and 0x200 (DPU_1) at M=64 K=256 N=256: which group signals
 * varies with the program, which is what the pair-collapsing is for. A mask of 0x100
 * would pass the small shape and hang on the large one.
 *
 * A WRONG mask is SILENT, not loud. The loud failure, "invalid irq status ... require
 * mask" and a job that never completes, only happens for a mask no block satisfies. Name
 * a block that DOES fire and the job retires early on that block instead: measured with
 * 0x3 (CNA_FEATURE) at 64x256x256, where the job's device time fell 121 us -> 79 us with
 * the output still correct at that size. That is measured EARLY RETIREMENT, not observed
 * corruption, but it is the RK3576 drain hazard's shape, where the fence signals while
 * the core is still writing and the output tail goes missing. Use the block that writes
 * the output.
 */
#define BSP_IRQ_DPU_PAIR 0x300u

/* The PPU pair, for a job whose LAST program is a pooling one. A pool enables no DPU
 * stage at all (PC_OPERATION_ENABLE is a per-block bitmap, and a pool sets 0x60 against
 * a convolution's 0x1d), so a DPU mask on such a job waits for a completion that never
 * comes. The driver takes INT_MASK from the job's LAST entry, which is exactly the block
 * that finishes it. */
#define BSP_IRQ_PPU_PAIR 0xc00u

/*
 * body_ops is NOT the regcmd entry count. Both drivers write the same physical register
 * (PC_DATA_AMOUNT, offset 0x14) by different formulas:
 *   mainline `rocket`: (regcmd_count + 1)/2 - 1
 *   `rknpu`:           (body_ops + 4 + scale-1)/scale - 1,
 *                      the 4 being a constant the driver adds, and scale its
 *                      pc_data_amount_scale, 2 on the RK3588
 * Equal register value => body_ops = regcmd_count - 4, for either parity.
 *
 * The 4 is not arbitrary: it is exactly the 4-op control trailer the generators emit at
 * the end of the program, which this uAPI expects the DRIVER to add back rather than the
 * caller to count. Passing regcmd_count straight through runs the program two DMA beats
 * long. [HW sweep + source-confirmed]
 */
#define BSP_TRAILER_OPS 4u

/* ---- The job (SUBMIT) ------------------------------------------------------ */
#define BSP_CMD_SUBMIT 0x01

/* Which entries one core runs. On a multi-core part the driver reads core c's range from
 * per_core[c] when the job names one or two cores, and from per_core[c + 2] when it
 * names all three; the five entries cover both. A single-core part reads first_task and
 * task_count instead. [source-confirmed: rknpu_job.c, 0.9.8] */
struct bsp_core_range {
    uint32_t first;
    uint32_t count;
};
_Static_assert(sizeof(struct bsp_core_range) == 8, "bsp_core_range size");
BSP_AT(bsp_core_range, first, 0);   BSP_AT(bsp_core_range, count, 4);

struct bsp_job {
    uint32_t mode;           /* in: BSP_JOB_* */
    uint32_t timeout_ms;     /* in */
    uint32_t first_task;     /* in: index into the task array */
    uint32_t task_count;     /* in */
    uint32_t tasks_done;     /* out: task_count on success; after a failed wait, the
                                hardware task counter */
    int32_t  priority;       /* in: 0 here */
    uint64_t task_cookie;    /* in: obj_cookie of the task array's buffer */
    uint32_t domain;         /* in: IOMMU domain; 0 here. The word section 5 is about */
    uint32_t pad;
    uint64_t pc_dma_base;    /* in: written to PC_DMA_BASE_ADDR; 0 here */
    int64_t  device_ns;      /* out: the job's elapsed time on the device, in ns */
    uint32_t cores;          /* in: BSP_CORE_ANY or BSP_CORE_BIT(n) */
    int32_t  fence;          /* in/out: a sync_file fd; -1 here, no fence asked for */
    struct bsp_core_range per_core[5];
};
_Static_assert(sizeof(struct bsp_job) == 104, "bsp_job size");
BSP_AT(bsp_job, mode, 0);           BSP_AT(bsp_job, timeout_ms, 4);
BSP_AT(bsp_job, first_task, 8);     BSP_AT(bsp_job, task_count, 12);
BSP_AT(bsp_job, tasks_done, 16);    BSP_AT(bsp_job, priority, 20);
BSP_AT(bsp_job, task_cookie, 24);   BSP_AT(bsp_job, domain, 32);
BSP_AT(bsp_job, pad, 36);           BSP_AT(bsp_job, pc_dma_base, 40);
BSP_AT(bsp_job, device_ns, 48);     BSP_AT(bsp_job, cores, 56);
BSP_AT(bsp_job, fence, 60);         BSP_AT(bsp_job, per_core, 64);

/* bsp_job.mode. BSP_JOB_VIA_PC has the PC block fetch the register program from memory,
 * and the driver refuses a job without it.
 *
 * BSP_JOB_WAIT is 0: waiting is the DEFAULT, and asking not to wait is a separate bit
 * (bit 1) that the provider never sets. So a submit through this uAPI returns only once
 * the job has retired, which is a STRONGER guarantee than the mainline `rocket` SUBMIT's
 * (async, waited on by PREP_BO). See the note on submit synchrony in rknpu_provider.c.
 */
#define BSP_JOB_VIA_PC  (1u << 0)
#define BSP_JOB_WAIT    0u

/* bsp_job.cores. With BSP_CORE_ANY the driver picks the least-loaded core and rewrites
 * the field to that core's bit. */
#define BSP_CORE_ANY     0x00u
#define BSP_CORE_BIT(n)  (1u << (n))

/* ===========================================================================
 * 4. Control (ACTION)
 *
 * One command multiplexes the driver's queries, settings and resets: `op` selects which,
 * and `data` is the argument of a set and the result of a get. The provider calls it for
 * the driver version, and rknpu_action() in include/rknpu_submit.h passes any other op
 * number through unchanged.
 * ======================================================================== */
#define BSP_CMD_CTL 0x00

struct bsp_ctl {
    uint32_t op;     /* in */
    uint32_t data;   /* in for a set, out for a get */
};
_Static_assert(sizeof(struct bsp_ctl) == 8, "bsp_ctl size");
BSP_AT(bsp_ctl, op, 0);   BSP_AT(bsp_ctl, data, 4);

#define BSP_CTL_DRIVER_VERSION 1u   /* get: BSP_VERSION(major, minor, patch) */

/* ===========================================================================
 * 5. Versions
 *
 * BSP_CTL_DRIVER_VERSION returns major*10000 + minor*100 + patch, so 0.9.8 reads 908.
 * The structures above are 0.9.6's, which 0.9.7 and 0.9.8 have unchanged, and two
 * thresholds bound where they apply.
 *
 * 0.9.6 (2024-03-18) grew the allocation structure (struct bsp_alloc) from 40 to 48
 * bytes by appending `domain` and a second 32-bit word, `cores` in 0.9.8. It also
 * re-purposed the 64-bit word at offset 32 of the job structure (struct bsp_job), which
 * older drivers declare as the address of a register-config object, into `domain` plus
 * `pad`. Every other structure holds its size from 0.8.0 through 0.9.8.
 *
 * The size change is not a boundary on the DRM render node. drm_ioctl() finds a driver
 * command by its number, copies in the size the request word encodes, zero-fills to the
 * larger of that and the driver's own size, and copies back the caller's size, so a
 * 0.9.5-or-older driver reads the 40-byte prefix it knows [source-confirmed:
 * drivers/gpu/drm/drm_ioctl.c]. The re-purposed submit field is the open difference: an
 * older driver reads the zero this provider writes as `domain` as a zero register-config
 * address, which other projects also pass and is likely ignored [hypothesis]. No driver
 * below 0.9.6 has run this provider, so 0.9.6 is a floor by validation.
 *
 * 0.9.7 (2024-04-24) added BSP_ALLOC_IOVA_TIGHT. The driver never checks the flags word
 * against the set of bits it defines, so an older one accepts the bit and ignores it:
 * the allocation takes the leaking generic route while the flag reads as set. The
 * provider withholds the bit below this version and says so once.
 * ======================================================================== */
#define BSP_VERSION(maj, min, patch) ((maj) * 10000 + (min) * 100 + (patch))
#define BSP_VERSION_MIN          BSP_VERSION(0, 9, 6)
#define BSP_VERSION_IOVA_TIGHT   BSP_VERSION(0, 9, 7)

#endif /* RKNPU_UAPI_H */
