// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The rknpu-submit authors
/*
 * rknpu_uapi.h — the vendor `rknpu` BSP driver's ioctl surface, transcribed.
 *
 * Transcribed rather than included: a BSP kernel does not install this header, and a
 * provider that only compiles where the vendor kernel tree happens to be unpacked is not
 * a provider anyone can build. Every struct below carries a _Static_assert on its size,
 * checked against the sizes the kernel's own definitions compile to on aarch64 and
 * x86-64, so a transcription error is a build failure and not a wrong ioctl.
 *
 * Source: drivers/rknpu/include/rknpu_ioctl.h, driver version 0.9.8 / 20240828, which is
 * what `[drm] Initialized rknpu 0.9.8 20240828` reports on the boards this was written
 * against. GET_DRV_VERSION should be read and gated on before trusting any of it —
 * the BSP driver versions independently of the kernel it ships in.
 */
#ifndef RKNPU_UAPI_H
#define RKNPU_UAPI_H

#include <stdint.h>
#include <sys/ioctl.h>

/* ---- DRM ioctl encoding (from drm/drm.h, transcribed for the same reason) ---- */
#define RKNPU_DRM_IOCTL_BASE    'd'
#define RKNPU_DRM_COMMAND_BASE  0x40

struct rknpu_drm_version {
    int    version_major;
    int    version_minor;
    int    version_patchlevel;
    size_t name_len;
    char  *name;
    size_t date_len;
    char  *date;
    size_t desc_len;
    char  *desc;
};
#define RKNPU_DRM_IOCTL_VERSION \
    _IOWR(RKNPU_DRM_IOCTL_BASE, 0x00, struct rknpu_drm_version)

/* ---- command numbers ------------------------------------------------------- */
#define RKNPU_ACTION_NR      0x00
#define RKNPU_SUBMIT_NR      0x01
#define RKNPU_MEM_CREATE_NR  0x02
#define RKNPU_MEM_MAP_NR     0x03
#define RKNPU_MEM_DESTROY_NR 0x04
#define RKNPU_MEM_SYNC_NR    0x05

/* ---- structs --------------------------------------------------------------- */
struct rknpu_action { uint32_t flags; uint32_t value; };

struct rknpu_mem_create {
    uint32_t handle;
    uint32_t flags;
    uint64_t size;
    uint64_t obj_addr;
    uint64_t dma_addr;
    uint64_t sram_size;
    int32_t  iommu_domain_id;
    uint32_t core_mask;
};

struct rknpu_mem_map { uint32_t handle; uint32_t reserved; uint64_t offset; };

struct rknpu_mem_destroy { uint32_t handle; uint32_t reserved; uint64_t obj_addr; };

struct rknpu_mem_sync {
    uint32_t flags; uint32_t reserved;
    uint64_t obj_addr; uint64_t offset; uint64_t size;
};

/*
 * The four task fields the mainline `rocket` task has no analogue for, decoded from
 * rknpu_job.c (0.9.8) rather than guessed:
 *
 *   enable_mask   never read by the driver.  Dead field.
 *   int_clear     never read by the driver — it writes first_task->int_mask to the
 *                 INT_CLEAR register, not int_clear.  Dead field.
 *   op_idx        unused on this path.
 *   regcfg_offset never read by the driver.  Dead field.
 *   int_status    written BACK by the driver; an OUTPUT, not an input.
 *   int_mask      the only live input.  See RKNPU_TASK_INT_MASK_DPU.
 */
struct rknpu_task {
    uint32_t flags;
    uint32_t op_idx;
    uint32_t enable_mask;
    uint32_t int_mask;
    uint32_t int_clear;
    uint32_t int_status;
    uint32_t regcfg_amount;
    uint32_t regcfg_offset;
    uint64_t regcmd_addr;
} __attribute__((packed));

struct rknpu_subcore_task { uint32_t task_start; uint32_t task_number; };

struct rknpu_submit {
    uint32_t flags;
    uint32_t timeout;
    uint32_t task_start;
    uint32_t task_number;
    uint32_t task_counter;
    int32_t  priority;
    uint64_t task_obj_addr;
    uint32_t iommu_domain_id;
    uint32_t reserved;
    uint64_t task_base_addr;
    int64_t  hw_elapse_time;
    uint32_t core_mask;
    int32_t  fence_fd;
    struct rknpu_subcore_task subcore_task[5];
};

_Static_assert(sizeof(struct rknpu_action)      ==  8, "rknpu_action layout");
_Static_assert(sizeof(struct rknpu_mem_create)  == 48, "rknpu_mem_create layout");
_Static_assert(sizeof(struct rknpu_mem_map)     == 16, "rknpu_mem_map layout");
_Static_assert(sizeof(struct rknpu_mem_destroy) == 16, "rknpu_mem_destroy layout");
_Static_assert(sizeof(struct rknpu_mem_sync)    == 32, "rknpu_mem_sync layout");
_Static_assert(sizeof(struct rknpu_task)        == 40, "rknpu_task layout");
_Static_assert(sizeof(struct rknpu_submit)      == 104, "rknpu_submit layout");

/* ---- flags ----------------------------------------------------------------- */
/* CONTIGUOUS is the absence of this bit, and asking for it is a trap on this driver.
 * rknpu_gem_alloc_buf() routes a NON_CONTIGUOUS request to rknpu_gem_get_pages(),
 * which populates the object's page array. A CONTIGUOUS request goes to
 * dma_alloc_attrs() with FORCE_CONTIGUOUS instead — and when CMA cannot satisfy it,
 * the driver falls back by setting NON_CONTIGUOUS on the object and re-allocating,
 * WITHOUT going back for the pages. mmap then sees the flag, takes
 * rknpu_gem_mmap_pages(), and __vm_map_pages() refuses against an unpopulated array:
 * 'failed to map pages into vma: -6'. So a contiguous allocation stops being mappable
 * the moment the CMA pool runs short — 256 MiB on the board this was found on, which a
 * resident-weight workload passes without trying.
 *
 * Asking for non-contiguous up front avoids both the CMA pressure and the fallback.
 * There is no cost: every buffer goes through the IOMMU, so the device sees one
 * contiguous IOVA range either way. [HW sweep + source-confirmed] */
#define RKNPU_MEM_NON_CONTIGUOUS   (1u << 0)
#define RKNPU_MEM_CACHEABLE        (1u << 1)
#define RKNPU_MEM_KERNEL_MAPPING   (1u << 3)
#define RKNPU_MEM_IOMMU            (1u << 4)
#define RKNPU_MEM_ZEROING          (1u << 5)

/* Without this flag the driver maps a BO with the kernel's generic
 * iommu_dma_map_sg(), whose IOVA allocator is alloc_iova_fast(size_aligned=true)
 * -> alloc_iova(), which does size = __roundup_pow_of_two(size) and then aligns
 * the result to that same boundary. So a BO one page over a power of two costs the
 * NEXT power of two of IOVA and must start on it. Our buffers are sized M*N*2 plus
 * tile padding, which lands just over a power of two as the common case, so the
 * waste is close to 2x and the alignment fragments what is left.
 *
 * Setting it takes the driver's own rknpu_iommu_dma_map_sg(), which allocates with
 * size_aligned=false: no roundup, no alignment. [HW sweep + source-confirmed] */
#define RKNPU_MEM_IOMMU_LIMIT_IOVA_ALIGNMENT (1u << 10)

#define RKNPU_MEM_SYNC_TO_DEVICE   (1u << 0)
#define RKNPU_MEM_SYNC_FROM_DEVICE (1u << 1)

/* RKNPU_JOB_BLOCK is 0 — blocking is the DEFAULT and NONBLOCK is the flag. So a
 * submit through this uAPI returns only once the job has retired, which is a
 * STRONGER guarantee than the mainline rocket SUBMIT's (async, waited on by
 * PREP_BO). See the note on submit synchrony in rknpu_provider.c. */
#define RKNPU_JOB_PC       (1u << 0)
#define RKNPU_JOB_BLOCK    0u
#define RKNPU_JOB_NONBLOCK (1u << 1)

#define RKNPU_CORE_AUTO_MASK 0x00u  /* the driver picks the least-loaded core */
#define RKNPU_CORE0_MASK   0x01u
#define RKNPU_CORE1_MASK   0x02u
#define RKNPU_CORE2_MASK   0x04u

/* ---- driver version ---------------------------------------------------------
 *
 * RKNPU_GET_DRV_VERSION returns MAJOR*10000 + MINOR*100 + PATCHLEVEL, so 0.9.8 reads
 * 908. The structures below are 0.9.6's, which 0.9.7 and 0.9.8 carry unchanged, and
 * two thresholds bound where they apply.
 *
 * 0.9.6 (2024-03-18) grew `rknpu_mem_create` from 40 to 48 bytes by appending
 * `iommu_domain_id` and its padding, and it re-purposed `rknpu_submit`'s 64-bit
 * `regcfg_obj_addr` into `iommu_domain_id` plus padding. The size change is the hard
 * boundary: the ioctl request word encodes the structure size, so a 0.9.5-or-older
 * driver does not recognize MEM_CREATE at all and the first allocation fails. Every
 * other structure holds its size from 0.8.0 through 0.9.8.
 *
 * 0.9.7 (2024-04-24) added RKNPU_MEM_IOMMU_LIMIT_IOVA_ALIGNMENT. The driver never
 * checks flags against RKNPU_MEM_MASK, so an older one accepts the bit and ignores it:
 * the allocation takes the leaking generic route while the flag reads as set. The
 * provider withholds the bit below this version and says so once.
 */
#define RKNPU_DRV_VERSION_CODE(maj, min, patch) ((maj) * 10000 + (min) * 100 + (patch))
#define RKNPU_DRV_VERSION_MIN         RKNPU_DRV_VERSION_CODE(0, 9, 6)
#define RKNPU_DRV_VERSION_IOVA_TIGHT  RKNPU_DRV_VERSION_CODE(0, 9, 7)

/* ---- actions (DRM_IOCTL_RKNPU_ACTION) -------------------------------------- */
#define RKNPU_GET_HW_VERSION        0
#define RKNPU_GET_DRV_VERSION       1
#define RKNPU_GET_FREQ              2
#define RKNPU_SET_FREQ              3
#define RKNPU_GET_VOLT              4
#define RKNPU_SET_VOLT              5
#define RKNPU_ACT_RESET             6
#define RKNPU_GET_BW_PRIORITY       7
#define RKNPU_SET_BW_PRIORITY       8
#define RKNPU_GET_BW_EXPECT         9
#define RKNPU_SET_BW_EXPECT        10
#define RKNPU_GET_BW_TW            11
#define RKNPU_SET_BW_TW            12
#define RKNPU_ACT_CLR_TOTAL_RW_AMOUNT 13
#define RKNPU_GET_DT_WR_AMOUNT     14
#define RKNPU_GET_DT_RD_AMOUNT     15
#define RKNPU_GET_WT_RD_AMOUNT     16
#define RKNPU_GET_TOTAL_RW_AMOUNT  17
#define RKNPU_GET_IOMMU_EN         18
#define RKNPU_SET_PROC_NICE        19
#define RKNPU_POWER_ON             20
#define RKNPU_POWER_OFF            21
#define RKNPU_GET_TOTAL_SRAM_SIZE  22
#define RKNPU_GET_FREE_SRAM_SIZE   23
#define RKNPU_GET_IOMMU_DOMAIN_ID  24
#define RKNPU_SET_IOMMU_DOMAIN_ID  25

/*
 * int_mask reaches the INT_MASK register, and the IRQ handler completes the job only
 * when rknpu_fuzz_status(status) == int_mask. fuzz_status collapses the status word
 * into block PAIRS (0x3 CNA_FEATURE, 0xc CNA_WEIGHT, 0x30 CNA_CSC, 0xc0 CORE,
 * 0x300 DPU, 0xc00 PPU), so the mask names which blocks must have signalled.
 *
 * Our programs terminate in a DPU write, and mainline rocket hard-codes exactly
 * PC_INTERRUPT_MASK_DPU_0|DPU_1 == 0x300 for the same programs; hello2.c's
 * IRQ_DPU_GROUP0/1 agree. Three independent sources, so this is not a guess.
 *
 * THE PAIR IS REQUIRED, not a single bit. Measured int_status came back 0x100 (DPU_0)
 * at M=4 K=32 N=16 and 0x200 (DPU_1) at M=64 K=256 N=256 — which group signals varies
 * with the program, which is what the pair-collapsing is for. A mask of 0x100 would
 * pass the small shape and hang on the large one.
 *
 * A WRONG mask is SILENT, not loud. The loud failure — "invalid irq status ... require
 * mask" and a job that never completes — only happens for a mask no block satisfies.
 * Name a block that DOES fire and the job retires early on that block instead:
 * measured with 0x3 (CNA_FEATURE) at 64x256x256, where hw_elapse_time fell 121 us ->
 * 79 us with the output still correct at that size. That is measured EARLY RETIREMENT,
 * not observed corruption — but it is the RK3576 drain hazard's shape, where the fence
 * signals while the core is still writing and the output tail goes missing. Use the
 * block that writes the output.
 */
#define RKNPU_TASK_INT_MASK_DPU  0x300u

/* The PPU pair, for a job whose LAST program is a pooling one. A pool enables no DPU
 * stage at all — PC_OPERATION_ENABLE is a per-block bitmap and a pool sets 0x60
 * against a convolution's 0x1d — so a DPU mask on such a job waits for a completion
 * that never comes. The driver takes INT_MASK from the job's LAST task, which is
 * exactly the block that finishes it. */
#define RKNPU_TASK_INT_MASK_PPU  0xc00u

/*
 * regcfg_amount is NOT the regcmd word count. Both drivers write the same physical
 * register (PC_DATA_AMOUNT, offset 0x14) by different formulas:
 *   rocket: (regcmd_count + 1)/2 - 1
 *   rknpu:  (regcfg_amount + EXTRA_AMOUNT + scale-1)/scale - 1,
 *           with EXTRA_AMOUNT = 4 and pc_data_amount_scale = 2 on RK3588
 * Equal register value => regcfg_amount = regcmd_count - 4, for either parity.
 *
 * The 4 is not arbitrary: it is exactly the 4-op control trailer the generators emit at
 * the end of the program, which this uAPI expects the DRIVER to add back rather than
 * the caller to count. Passing regcmd_count straight through runs the program two DMA
 * beats long. [HW sweep + source-confirmed]
 */
#define RKNPU_REGCFG_EXTRA 4u

#endif /* RKNPU_UAPI_H */
