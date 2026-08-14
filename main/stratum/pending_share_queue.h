#pragma once

#include <stdint.h>
#include <string.h>
#include <pthread.h>

#include "macros.h"

#define PENDING_SHARE_MAX 16
#define PENDING_SHARE_TTL_US 60000000LL

struct PendingShare {
    int pool = 0;
    char *jobid = nullptr;
    char *extranonce2 = nullptr;
    uint32_t ntime = 0;
    uint32_t nonce = 0;
    uint32_t version_rolled = 0;
    uint32_t version_base = 0;
    double nonce_diff = 0;
    int64_t queued_us = 0;
    bool used = false;
};

class PendingShareQueue {
    PendingShare m_shares[PENDING_SHARE_MAX]{};
    pthread_mutex_t m_mutex = PTHREAD_MUTEX_INITIALIZER;

    static void clearShare(PendingShare &share)
    {
        safe_free(share.jobid);
        safe_free(share.extranonce2);
        share.used = false;
        share.nonce_diff = 0;
        share.queued_us = 0;
        share.nonce = 0;
        share.pool = 0;
    }

    static void fillShare(PendingShare &dst, int pool, const char *jobid, const char *extranonce_2, uint32_t ntime,
                          uint32_t nonce, uint32_t version_rolled, uint32_t version_base, double nonce_diff,
                          int64_t queued_us)
    {
        dst.pool = pool;
        dst.jobid = jobid ? strdup(jobid) : nullptr;
        dst.extranonce2 = extranonce_2 ? strdup(extranonce_2) : nullptr;
        dst.ntime = ntime;
        dst.nonce = nonce;
        dst.version_rolled = version_rolled;
        dst.version_base = version_base;
        dst.nonce_diff = nonce_diff;
        dst.queued_us = queued_us;
        dst.used = true;
    }

  public:
    ~PendingShareQueue()
    {
        for (int i = 0; i < PENDING_SHARE_MAX; i++) {
            clearShare(m_shares[i]);
        }
    }

    // Returns false if the share was dropped (queue full of higher-diff work).
    bool enqueue(int pool, const char *jobid, const char *extranonce_2, uint32_t ntime, uint32_t nonce,
                 uint32_t version_rolled, uint32_t version_base, double nonce_diff, int64_t now_us,
                 int64_t queued_us = 0)
    {
        PThreadGuard lock(m_mutex);

        int slot = -1;
        for (int i = 0; i < PENDING_SHARE_MAX; i++) {
            if (!m_shares[i].used) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < PENDING_SHARE_MAX; i++) {
                if (m_shares[i].nonce_diff < m_shares[slot].nonce_diff) {
                    slot = i;
                }
            }
            if (m_shares[slot].nonce_diff >= nonce_diff) {
                return false;
            }
            clearShare(m_shares[slot]);
        }

        fillShare(m_shares[slot], pool, jobid, extranonce_2, ntime, nonce, version_rolled, version_base, nonce_diff,
                  queued_us ? queued_us : now_us);
        return true;
    }

    // Moves live (non-expired) shares into out[]. Caller owns jobid/extranonce2.
    int takeReady(PendingShare *out, int max, int64_t now_us)
    {
        PThreadGuard lock(m_mutex);
        int count = 0;
        for (int i = 0; i < PENDING_SHARE_MAX && count < max; i++) {
            if (!m_shares[i].used) {
                continue;
            }
            if (now_us - m_shares[i].queued_us > PENDING_SHARE_TTL_US) {
                clearShare(m_shares[i]);
                continue;
            }
            out[count] = m_shares[i];
            m_shares[i].used = false;
            m_shares[i].jobid = nullptr;
            m_shares[i].extranonce2 = nullptr;
            count++;
        }
        return count;
    }

    int usedCount()
    {
        PThreadGuard lock(m_mutex);
        int n = 0;
        for (int i = 0; i < PENDING_SHARE_MAX; i++) {
            if (m_shares[i].used) {
                n++;
            }
        }
        return n;
    }

    bool containsNonce(uint32_t nonce)
    {
        PThreadGuard lock(m_mutex);
        for (int i = 0; i < PENDING_SHARE_MAX; i++) {
            if (m_shares[i].used && m_shares[i].nonce == nonce) {
                return true;
            }
        }
        return false;
    }

    double maxQueuedDiff()
    {
        PThreadGuard lock(m_mutex);
        double best = 0;
        for (int i = 0; i < PENDING_SHARE_MAX; i++) {
            if (m_shares[i].used && m_shares[i].nonce_diff > best) {
                best = m_shares[i].nonce_diff;
            }
        }
        return best;
    }
};
