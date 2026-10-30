// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The rknpu-submit authors
/*
 * rknpu_submit.h: the measurement functions this provider offers in addition to the
 * librocketnpu submit seam.
 *
 * The seam itself is <rocket_npu.h> and is unchanged. A consumer links this provider
 * instead of the mainline one and calls the same functions. The functions below reach
 * measurements that the rknpu uAPI has and the mainline `accel/rocket` uAPI does not. A
 * measurement harness reads them through the provider's own device fd.
 *
 * librocketnpu, the op library, graph/ and the frontends must not call these. They are
 * compiled against providers that do not define them, and a caller that uses them does not
 * build against the mainline provider.
 *
 * The rknpu driver does not implement the DMA byte counters or the SRAM and NBUF pools on
 * the RK3588. Its rk3588_rknpu_config sets .amount_top = NULL, .amount_core = NULL and
 * .nbuf_size = 0. A counter action therefore returns 0 and leaves *value as the caller
 * passed it. The driver logs "Get rw_amount is not supported on this device!", or "Get
 * total_rw_amount ..." for the total. Both SRAM queries return 0, and TRY_ALLOC_SRAM/_NBUF
 * fall back to DDR and grant sram_size = 0.
 *
 * Bandwidth QoS is not implemented there either: the three GET_BW_* actions return EINVAL
 * against .bw_priority_addr = 0x0. Other parts' configurations differ. The byte counters
 * are implemented on RK3576, rk356x, rk3562, rv1106 and rv1126b, the NBUF pool on RK3576,
 * rk3562 and rv1126b, and bandwidth QoS on rk356x. The SRAM pool comes from the device tree.
 * [HW sweep + source-confirmed]
 *
 * On an RK3588, the measurements available are the hardware elapsed time per submit, the
 * clock and voltage queries, and the IOMMU domain id.
 */
#ifndef RKNPU_SUBMIT_H
#define RKNPU_SUBMIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware elapsed time of the last submit on this fd, in nanoseconds, as the driver
 * wrote it back into rknpu_submit.hw_elapse_time. It separates device time from dispatch
 * and host time without an external timer: measured 31-121 us across small and large
 * matmul shapes. Returns -1 for an fd this provider does not know.
 *
 * Not thread-safe. The value is per fd, not per submit: it is the last value written, so
 * read it immediately after the submit whose time you want. */
int64_t rknpu_last_hw_elapse_ns(int fd);

/* Summed hardware elapsed time over every submit this process has made since the last
 * rocket_submit_counters_reset(), in nanoseconds. *n_submits receives how many submits
 * contributed. A tiled or multicore shape needs this sum. Each submit overwrites the per-fd
 * value above, so that value reports only the last kick of a many-kick matmul.
 *
 * The value is a ktime pair, not a hardware counter [source-confirmed, rknpu_job.c]. The
 * driver stamps hw_commit_time where it pulls the job off the core's todo list, immediately
 * before it writes the register program. It takes the delta in the IRQ path. The value
 * therefore includes the register-write burst and the interrupt latency. It excludes the
 * ioctl entry, the BO and IOMMU setup, the fence handling, and every host-side cost. The
 * packing and de-tiling that dominate this part's wall-clock time are host-side costs.
 *
 * Read these bounds before quoting a dispatch overhead from the difference between this sum
 * and wall-clock time.
 *
 * Two more bounds, neither visible from the value:
 *
 * On a multicore job, each core's completion overwrites the job's elapsed time rather than
 * adding to it. A three-core submit therefore contributes one core's span, not their sum.
 *
 * The driver commits a job with more than max_submit_number tasks (4095 on RK3588) in
 * chunks, without re-stamping hw_commit_time. Such a job's contribution spans every chunk
 * from the first commit. The task counts this library emits stay below that limit.
 *
 * Submits the driver never committed write back 0 and are counted in neither sum. */
uint64_t rknpu_hw_elapse_total_ns(uint64_t *n_submits);

/* Raw DRM_IOCTL_RKNPU_ACTION. `action` is one of the rknpu uAPI's RKNPU_GET_ / RKNPU_SET_
 * numbers. The provider's src/rknpu_uapi.h transcribes them and is not installed. *value
 * is the input for a SET and receives the result for a GET.
 * Returns 0, or a negative errno. The driver returns EINVAL for an action this part does
 * not implement. That EINVAL is an answer about the part, not a failure of this call.
 *
 * The counter actions wrap at 4 GB of traffic (rknpu_action.value is __u32). Clear them
 * with RKNPU_ACT_CLR_TOTAL_RW_AMOUNT before each measurement, rather than differencing two
 * reads across an unknown amount of work. */
int rknpu_action(int fd, uint32_t action, uint32_t *value);

#ifdef __cplusplus
}
#endif

#endif /* RKNPU_SUBMIT_H */
