#pragma once

#include "mining.h"

// Choose active vs retired job for an ASIC nonce.
// active_diff / retired_diff are test_nonce_value() results (0 if that job is missing).
// Frees the unused clone. Returns the job to submit, or nullptr.
inline bm_job *select_job_for_nonce(bm_job *active, bm_job *retired, double active_diff, double retired_diff,
                                    double *out_diff)
{
    if (active && retired && active_diff < (double) active->asic_diff && retired_diff >= (double) retired->asic_diff) {
        free_bm_job(active);
        if (out_diff) {
            *out_diff = retired_diff;
        }
        return retired;
    }
    if (retired && !active) {
        if (out_diff) {
            *out_diff = retired_diff;
        }
        return retired;
    }
    if (retired) {
        free_bm_job(retired);
    }
    if (out_diff) {
        *out_diff = active_diff;
    }
    return active;
}
