#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asic_jobs.h"
#include "asic_job_select.h"
#include "pending_share_queue.h"

static int g_failed = 0;
static int g_passed = 0;

#define EXPECT(cond, msg)                                                                                                          \
    do {                                                                                                                           \
        if (!(cond)) {                                                                                                             \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                                 \
            g_failed++;                                                                                                            \
        } else {                                                                                                                   \
            g_passed++;                                                                                                            \
        }                                                                                                                          \
    } while (0)

static bm_job *make_job(const char *jobid, const char *en2, int pool, uint32_t asic_diff, uint32_t ntime)
{
    bm_job *job = (bm_job *) calloc(1, sizeof(bm_job));
    job->jobid = strdup(jobid);
    job->extranonce2 = strdup(en2);
    job->pool_id = pool;
    job->asic_diff = asic_diff;
    job->pool_diff = 1500000;
    job->ntime = ntime;
    job->version = 0x20000000;
    return job;
}

static void test_clean_jobs_keeps_retired()
{
    AsicJobs jobs;
    jobs.storeJob(make_job("job-a", "aa", 0, 1024, 100), 0x18);

    bm_job *active = jobs.getClone(0x18);
    EXPECT(active != nullptr, "active clone before clean");
    EXPECT(strcmp(active->jobid, "job-a") == 0, "active job id");
    free_bm_job(active);

    int n = jobs.cleanJobs(0);
    EXPECT(n == 1, "cleanJobs retired one slot");

    EXPECT(jobs.getClone(0x18) == nullptr, "active gone after clean");
    bm_job *retired = jobs.getRetiredClone(0x18);
    EXPECT(retired != nullptr, "retired clone after clean");
    EXPECT(strcmp(retired->jobid, "job-a") == 0, "retired keeps job-a");
    EXPECT(strcmp(retired->extranonce2, "aa") == 0, "retired keeps extranonce2");
    free_bm_job(retired);
}

static void test_overwrite_retires_previous()
{
    AsicJobs jobs;
    jobs.storeJob(make_job("old", "01", 0, 1024, 1), 0x18);
    jobs.storeJob(make_job("new", "02", 0, 1024, 2), 0x18);

    bm_job *active = jobs.getClone(0x18);
    bm_job *retired = jobs.getRetiredClone(0x18);
    EXPECT(active && strcmp(active->jobid, "new") == 0, "active is new job");
    EXPECT(retired && strcmp(retired->jobid, "old") == 0, "retired is old job");
    free_bm_job(active);
    free_bm_job(retired);
}

static void test_clean_other_pool_leaves_job()
{
    AsicJobs jobs;
    jobs.storeJob(make_job("p0", "00", 0, 1024, 1), 0x10);
    jobs.storeJob(make_job("p1", "11", 1, 1024, 1), 0x20);

    EXPECT(jobs.cleanJobs(0) == 1, "cleaned pool 0 only");
    EXPECT(jobs.getClone(0x10) == nullptr, "pool 0 active cleared");
    bm_job *p0r = jobs.getRetiredClone(0x10);
    EXPECT(p0r && strcmp(p0r->jobid, "p0") == 0, "pool 0 retired");
    bm_job *p1 = jobs.getClone(0x20);
    EXPECT(p1 && strcmp(p1->jobid, "p1") == 0, "pool 1 still active");
    free_bm_job(p0r);
    free_bm_job(p1);
}

static void test_select_prefers_retired_when_active_misses()
{
    bm_job *active = make_job("active", "a", 0, 1024, 1);
    bm_job *retired = make_job("retired", "r", 0, 1024, 1);
    double out = 0;
    bm_job *picked = select_job_for_nonce(active, retired, 10.0, 5e14, &out);
    EXPECT(picked != nullptr, "picked a job");
    EXPECT(strcmp(picked->jobid, "retired") == 0, "block nonce recovered from retired");
    EXPECT(out == 5e14, "diff is retired diff");
    free_bm_job(picked);
}

static void test_select_keeps_active_when_it_matches()
{
    bm_job *active = make_job("active", "a", 0, 1024, 1);
    bm_job *retired = make_job("retired", "r", 0, 1024, 1);
    double out = 0;
    bm_job *picked = select_job_for_nonce(active, retired, 2048.0, 5e14, &out);
    EXPECT(picked && strcmp(picked->jobid, "active") == 0, "active wins when it meets asic_diff");
    EXPECT(out == 2048.0, "diff is active diff");
    free_bm_job(picked);
}

static void test_select_retired_only_after_clean()
{
    bm_job *retired = make_job("retired", "r", 0, 1024, 1);
    double out = 0;
    bm_job *picked = select_job_for_nonce(nullptr, retired, 0, 5e14, &out);
    EXPECT(picked && strcmp(picked->jobid, "retired") == 0, "retired-only after cleanJobs");
    EXPECT(out == 5e14, "500T diff preserved");
    free_bm_job(picked);
}

static void test_queue_retry_and_flush()
{
    PendingShareQueue q;
    EXPECT(q.enqueue(0, "jid", "en2", 1, 0xabc, 2, 3, 1500000, 1000), "enqueue share");
    EXPECT(q.usedCount() == 1, "one pending");
    EXPECT(q.containsNonce(0xabc), "nonce queued");

    PendingShare out[PENDING_SHARE_MAX];
    int n = q.takeReady(out, PENDING_SHARE_MAX, 2000);
    EXPECT(n == 1, "flush took one");
    EXPECT(out[0].nonce == 0xabc, "flushed nonce");
    EXPECT(strcmp(out[0].jobid, "jid") == 0, "flushed jobid");
    EXPECT(q.usedCount() == 0, "queue empty after take");
    safe_free(out[0].jobid);
    safe_free(out[0].extranonce2);
}

static void test_queue_keeps_block_when_full()
{
    PendingShareQueue q;
    for (int i = 0; i < PENDING_SHARE_MAX; i++) {
        EXPECT(q.enqueue(0, "low", "e", 1, 1000 + i, 0, 0, 1500000.0, 1000), "fill queue with low-diff");
    }
    EXPECT(q.usedCount() == PENDING_SHARE_MAX, "queue full");

    EXPECT(q.enqueue(0, "block", "e", 1, 0xdeadbeef, 0, 0, 5e14, 1000), "500T share accepted");
    EXPECT(q.containsNonce(0xdeadbeef), "block nonce kept");
    EXPECT(q.maxQueuedDiff() == 5e14, "max diff is 500T");
    EXPECT(q.usedCount() == PENDING_SHARE_MAX, "still full after replace");

    EXPECT(!q.enqueue(0, "tiny", "e", 1, 0x111, 0, 0, 100.0, 1000), "lower-diff share dropped");
    EXPECT(!q.containsNonce(0x111), "tiny nonce not stored");
}

static void test_queue_ttl_expires()
{
    PendingShareQueue q;
    q.enqueue(0, "old", "e", 1, 0x55, 0, 0, 1500000, 0, 1000);
    PendingShare out[PENDING_SHARE_MAX];
    int n = q.takeReady(out, PENDING_SHARE_MAX, 1000 + PENDING_SHARE_TTL_US + 1);
    EXPECT(n == 0, "expired share not flushed");
    EXPECT(q.usedCount() == 0, "expired share cleared");
}

static void test_queue_preserves_ttl_on_requeue()
{
    PendingShareQueue q;
    q.enqueue(0, "jid", "e", 1, 0x42, 0, 0, 5e14, 5000, 1000);
    PendingShare out[1];
    int n = q.takeReady(out, 1, 5000);
    EXPECT(n == 1, "took share");
    EXPECT(out[0].queued_us == 1000, "original queued_us kept");
    q.enqueue(0, out[0].jobid, out[0].extranonce2, out[0].ntime, out[0].nonce, out[0].version_rolled, out[0].version_base,
              out[0].nonce_diff, 5000, out[0].queued_us);
    safe_free(out[0].jobid);
    safe_free(out[0].extranonce2);

    n = q.takeReady(out, 1, 1000 + PENDING_SHARE_TTL_US + 1);
    EXPECT(n == 0, "requeued share still expires from original time");
}

static void test_submit_retry_simulation()
{
    PendingShareQueue q;
    int send_attempts = 0;
    auto try_send = [&](bool succeed) {
        send_attempts++;
        return succeed;
    };

    bool sent = false;
    for (int i = 0; i < 3 && !sent; i++) {
        sent = try_send(false);
    }
    EXPECT(!sent, "immediate retries failed");
    EXPECT(q.enqueue(0, "block", "e2", 99, 0x500, 1, 2, 5e14, 10), "queued after retries");

    PendingShare out[1];
    int n = q.takeReady(out, 1, 20);
    EXPECT(n == 1, "flush after reconnect");
    sent = try_send(true);
    EXPECT(sent, "retry after reconnect succeeds");
    EXPECT(out[0].nonce == 0x500, "same nonce submitted");
    EXPECT(out[0].nonce_diff == 5e14, "500T diff intact");
    safe_free(out[0].jobid);
    safe_free(out[0].extranonce2);
    EXPECT(send_attempts == 4, "3 fails + 1 success");
}

int main()
{
    test_clean_jobs_keeps_retired();
    test_overwrite_retires_previous();
    test_clean_other_pool_leaves_job();
    test_select_prefers_retired_when_active_misses();
    test_select_keeps_active_when_it_matches();
    test_select_retired_only_after_clean();
    test_queue_retry_and_flush();
    test_queue_keeps_block_when_full();
    test_queue_ttl_expires();
    test_queue_preserves_ttl_on_requeue();
    test_submit_retry_simulation();

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
