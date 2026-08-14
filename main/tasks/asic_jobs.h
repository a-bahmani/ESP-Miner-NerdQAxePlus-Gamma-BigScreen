#pragma once

#include <pthread.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "macros.h"
#include "mining.h"

#define MAX_ASIC_JOBS 128

class AsicJobs {
protected:
    bm_job *m_activeJobs[MAX_ASIC_JOBS];
    // Previous generation: survives cleanJobs() and slot overwrite so a late
    // ASIC nonce (including a block) can still be submitted.
    bm_job *m_retiredJobs[MAX_ASIC_JOBS];
    pthread_mutex_t m_validJobsLock;

    static uint8_t slotOf(uint8_t asic_job_id)
    {
        return asic_job_id & (MAX_ASIC_JOBS - 1);
    }

    void retireLocked(uint8_t slot)
    {
        if (m_retiredJobs[slot]) {
            free_bm_job(m_retiredJobs[slot]);
        }
        m_retiredJobs[slot] = m_activeJobs[slot];
        m_activeJobs[slot] = nullptr;
    }

    bm_job *cloneBmJob(bm_job *src)
    {
        if (!src) {
            return nullptr;
        }

        bm_job *dst = (bm_job *) malloc(sizeof(bm_job));
        if (!dst) {
            return nullptr;
        }

        memcpy(dst, src, sizeof(bm_job));
        dst->extranonce2 = src->extranonce2 ? strdup(src->extranonce2) : nullptr;
        dst->jobid = src->jobid ? strdup(src->jobid) : nullptr;
        return dst;
    }

public:
    AsicJobs() {
        m_validJobsLock = PTHREAD_MUTEX_INITIALIZER;
        memset(m_activeJobs, 0, sizeof(m_activeJobs));
        memset(m_retiredJobs, 0, sizeof(m_retiredJobs));
    }

    int cleanJobs(int pool) {
        PThreadGuard g(m_validJobsLock);
        int deleted = 0;
        for (int i = 0; i < MAX_ASIC_JOBS; i++) {
            if (m_activeJobs[i] && m_activeJobs[i]->pool_id == pool) {
                retireLocked((uint8_t) i);
                deleted++;
            }
        }
        return deleted;
    }

    void storeJob(bm_job *next_job, uint8_t asic_job_id) {
        PThreadGuard g(m_validJobsLock);
        uint8_t slot = slotOf(asic_job_id);
        if (m_activeJobs[slot]) {
            retireLocked(slot);
        }
        m_activeJobs[slot] = next_job;
    }

    bm_job *getClone(uint8_t asic_job_id) {
        PThreadGuard g(m_validJobsLock);
        return cloneBmJob(m_activeJobs[slotOf(asic_job_id)]);
    }

    bm_job *getRetiredClone(uint8_t asic_job_id) {
        PThreadGuard g(m_validJobsLock);
        return cloneBmJob(m_retiredJobs[slotOf(asic_job_id)]);
    }

};
