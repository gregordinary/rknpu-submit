// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The rknpu-submit authors
/*
 * rknpu_submit.h — what this provider offers BEYOND the librocketnpu submit seam.
 *
 * The seam itself is <rocket_npu.h> and is unchanged: a consumer links this provider
 * instead of the mainline one and calls the same functions. What follows is the small
 * set of instruments the vendor uAPI carries and the mainline `accel/rocket` one does
 * not, exposed so a measurement harness can reach them without a second device fd.
 *
 * NOTHING ABOVE THE SEAM MAY CALL THESE. librocketnpu, the op library, graph/ and the
 * frontends are all compiled against providers that do not have them; a caller that
 * reaches in here is a harness, and it will not build against the mainline provider.
 *
 * A WARNING ABOUT WHICH PART YOU ARE ON. The two instruments most worth having — the
 * DMA byte counters and the SRAM/NBUF pools — are NOT WIRED FOR RK3588. Its
 * rk3588_rknpu_config sets .amount_top = NULL / .amount_core = NULL and .nbuf_size = 0,
 * so all four counters read 0 (the driver logs "Get rw_amount is not supported on this
 * device!") and both SRAM queries return 0, with TRY_ALLOC_SRAM/_NBUF falling back to
 * DDR and granting sram_size = 0. Bandwidth QoS is likewise unimplemented there: the
 * three GET_BW_* actions return EINVAL against .bw_priority_addr = 0x0. They ARE wired
 * on RK3576, rk356x, rk3562, rv1106 and rv1126b. [HW sweep + source-confirmed]
 *
 * So on an RK3588 the instruments that survive are hardware elapsed time per submit,
 * the clock and voltage queries, and the IOMMU domain id.
 */
#ifndef RKNPU_SUBMIT_H
#define RKNPU_SUBMIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware elapsed time of the LAST submit on this fd, in nanoseconds, as the driver
 * wrote it back into rknpu_submit.hw_elapse_time. Separates device time from
 * dispatch/host time without an external timer — measured 31-121 us across small and
 * large matmul shapes. Returns -1 for an fd this provider does not know.
 *
 * Not thread-safe and per-fd, not per-submit: it is the last value written, so read it
 * immediately after the submit whose time you want. */
int64_t rknpu_last_hw_elapse_ns(int fd);

/* Summed hardware elapsed time over every submit this PROCESS has made since the last
 * rocket_submit_counters_reset(), in nanoseconds; *n_submits receives how many submits
 * contributed. This is what a tiled or multicore shape needs — the per-fd value above is
 * overwritten by each submit, so it reports only the last kick of a many-kick matmul.
 *
 * Read the envelope before quoting a dispatch floor from the difference between this and
 * a wall clock [source-confirmed, rknpu_job.c]. It is a ktime PAIR, not a hardware
 * counter: hw_commit_time is stamped where the job is pulled off the core's todo list,
 * immediately before the register program is written, and the delta is taken in the IRQ
 * path. So it CONTAINS the register-write burst and the interrupt latency, and EXCLUDES
 * the ioctl entry, the BO and IOMMU setup, the fence plumbing, and every host-side cost —
 * including the packing and de-tiling that dominate this part's wall.
 *
 * Two more bounds, neither of them visible from the value. On a MULTICORE job each core's
 * completion overwrites the job's elapsed time rather than adding to it, so a three-core
 * submit contributes one core's span and not their sum. And a job carrying more than
 * max_submit_number tasks (4095 on RK3588) is re-committed in chunks, re-stamping
 * hw_commit_time each time, so its contribution covers only the final chunk — inert at
 * the task counts this library emits, and not inert if that ever changes.
 *
 * Submits the driver never committed write back 0 and are counted in neither sum. */
uint64_t rknpu_hw_elapse_total_ns(uint64_t *n_submits);

/* Raw DRM_IOCTL_RKNPU_ACTION. `action` is one of the RKNPU_GET_ / RKNPU_SET_ numbers
 * in src/rknpu_uapi.h; *value is the input for a SET and receives the result for a GET.
 * Returns 0, or a negative errno — EINVAL is what an action this part does not
 * implement returns, which is a real answer and not a failure of this call.
 *
 * The counter actions wrap at 4 GB of traffic (rknpu_action.value is __u32), so clear
 * with RKNPU_ACT_CLR_TOTAL_RW_AMOUNT before each measurement rather than differencing
 * two reads across an unknown amount of work. */
int rknpu_action(int fd, uint32_t action, uint32_t *value);

#ifdef __cplusplus
}
#endif

#endif /* RKNPU_SUBMIT_H */
