#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    uint32_t version;
    uint32_t version_mask;
    uint8_t prev_block_hash[32];
    uint8_t prev_block_hash_be[32];
    uint8_t merkle_root[32];
    uint8_t merkle_root_be[32];
    uint32_t ntime;
    uint32_t target;
    uint32_t starting_nonce;
    uint32_t pool_diff;
    uint32_t asic_diff;
    int pool_id;
    char *jobid;
    char *extranonce2;
} bm_job;

static inline void free_bm_job(bm_job *job)
{
    if (!job) {
        return;
    }
    free(job->jobid);
    free(job->extranonce2);
    free(job);
}
