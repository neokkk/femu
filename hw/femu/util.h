// obj_trace_store.h — QEMU/GLib 가정
#pragma once
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

/* ===== 시간 유틸 ===== */
static inline uint64_t nsec_now_mono(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static inline uint64_t nsec_now_real(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ===== 트레이스 객체 (create → full → reclaim) ===== */
typedef struct obj_trace_t {
    int64_t  id;              // 같은 id 재사용 가능(단, 활성 중엔 중복 없음)
    uint16_t ruhid;
    uint64_t t_create_ns;     // MONOTONIC
    uint64_t t_full_ns;       // 0이면 미기록
    uint64_t t_reclaim_ns;    // 0이면 미기록 (이 시점에 활성 종료)
    uint64_t t_wall_create_ns;// 옵션: 가독용 벽시계
    uint32_t valid_pgs;
    bool gc;
} obj_trace_t;

/* ===== 저장소: 누적 배열 + 활성 맵 ===== */
typedef struct {
    GArray     *accum;        // 누적 저장(삭제 없음)
    GHashTable *active;       // key: int64_t* → val: GUINT_TO_POINTER(index in accum)
} ObjTraceStore;

/* 내부: int64 키 복제 */
static inline gpointer key_dup_i64(int64_t id) {
    int64_t *p = g_new(int64_t, 1); *p = id; return p;
}

/* ===== API ===== */
static inline void objt_store_init(ObjTraceStore *st, guint reserved)
{
    g_return_if_fail(st);
    st->accum  = g_array_sized_new(FALSE, FALSE, sizeof(obj_trace_t), MAX(8, reserved));
    st->active = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
}

static inline void objt_store_destroy(ObjTraceStore *st)
{
    if (!st) return;
    if (st->active) { g_hash_table_destroy(st->active); st->active = NULL; }
    if (st->accum)  { g_array_free(st->accum, TRUE);    st->accum  = NULL; }
}

/* create: 새로 누적에 append + active 등록 (활성 중복 금지) */
static inline bool objt_on_create(ObjTraceStore *st, int64_t id, uint16_t ruhid, bool gc)
{
    g_return_val_if_fail(st && st->accum && st->active, false);
    if (g_hash_table_contains(st->active, &id)) return false; // 활성 충돌
    obj_trace_t tr = {
        .id = id,
        .ruhid = ruhid,
        .t_create_ns      = nsec_now_mono(),
        .t_full_ns        = 0,
        .t_reclaim_ns     = 0,
        .t_wall_create_ns = nsec_now_real(),
        .gc               = gc,
    };
    g_array_append_val(st->accum, tr);
    guint idx = st->accum->len - 1;
    g_hash_table_insert(st->active, key_dup_i64(id), GUINT_TO_POINTER(idx));
    return true;
}

/* full: 활성에서 찾아 t_full 채움(1회) */
static inline bool objt_on_full(ObjTraceStore *st, int64_t id)
{
    g_return_val_if_fail(st && st->accum && st->active, false);
    gpointer val = g_hash_table_lookup(st->active, &id);
    if (!val) return false;
    guint idx = GPOINTER_TO_UINT(val);
    obj_trace_t *tr = &g_array_index(st->accum, obj_trace_t, idx);
    if (tr->t_full_ns == 0) tr->t_full_ns = nsec_now_mono();
    return true;
}

/* reclaim: 활성에서 찾아 t_reclaim 채우고 active에서 제거(누적에는 남김) */
static inline bool objt_on_reclaim(ObjTraceStore *st, int64_t id, int valid_pgs)
{
    g_return_val_if_fail(st && st->accum && st->active, false);
    gpointer val = NULL, orig_key = NULL;
    if (!g_hash_table_lookup_extended(st->active, &id, &orig_key, &val)) return false;
    guint idx = GPOINTER_TO_UINT(val);
    obj_trace_t *tr = &g_array_index(st->accum, obj_trace_t, idx);
    if (tr->t_reclaim_ns == 0) tr->t_reclaim_ns = nsec_now_mono();
    tr->valid_pgs = valid_pgs;
    g_hash_table_remove(st->active, &id); // 활성 종료
    return true;
}

/* 활성에서 id로 현재 obj 포인터 얻기(없으면 NULL) */
static inline obj_trace_t* objt_active_lookup(ObjTraceStore *st, int64_t id)
{
    g_return_val_if_fail(st && st->accum && st->active, NULL);
    gpointer val = g_hash_table_lookup(st->active, &id);
    if (!val) return NULL;
    guint idx = GPOINTER_TO_UINT(val);
    return &g_array_index(st->accum, obj_trace_t, idx);
}

/* 카운트 */
static inline guint objt_count_total(const ObjTraceStore *st)
{
    g_return_val_if_fail(st && st->accum, 0);
    return st->accum->len;
}
static inline guint objt_count_active(const ObjTraceStore *st)
{
    g_return_val_if_fail(st && st->active, 0);
    return g_hash_table_size(st->active);
}

/* 누적 덤프(CSV). create→full, full→reclaim, 전체 수명 포함 */
static inline void objt_dump_csv(const ObjTraceStore *st, FILE *fp)
{
    g_return_if_fail(st && st->accum && fp);
    fprintf(fp,
        "id,ruhid,copied_pgs,t_create_ns,t_full_ns,t_reclaim_ns,t_wall_create_ns,"
        "create_to_full_ms,full_to_reclaim_ms,lifecycle_ms,gc\n");
    for (guint i = 0; i < st->accum->len; i++) {
        const obj_trace_t *tr = &g_array_index(st->accum, obj_trace_t, i);
        double c2f_ms = (tr->t_full_ns     ? (double)(tr->t_full_ns     - tr->t_create_ns)/1e6 : 0.0);
        double f2r_ms = (tr->t_full_ns && tr->t_reclaim_ns) ? (double)(tr->t_reclaim_ns - tr->t_full_ns)/1e6 : 0.0;
        double life_ms= (tr->t_reclaim_ns  ? (double)(tr->t_reclaim_ns  - tr->t_create_ns)/1e6 : 0.0);
        fprintf(fp, "%" PRId64 "%d,%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%.3f,%.3f,%d\n",
                tr->id, tr->ruhid, tr->valid_pgs, tr->t_create_ns, tr->t_full_ns, tr->t_reclaim_ns, tr->t_wall_create_ns,
                c2f_ms, f2r_ms, life_ms, tr->gc);
    }
}

#ifndef LOG_CHUNK_SIZE
#define LOG_CHUNK_SIZE (64 * 1024)  // 64KB per chunk (원하면 256KB/1MB로 조정)
#endif

typedef struct LogChunk {
    struct LogChunk *next;
    size_t len;   // 사용 중 길이
    size_t cap;   // data 용량
    char data[];  // 가변 배열
} LogChunk;

typedef struct {
    LogChunk *g_head;
    LogChunk *g_tail;
    size_t g_total;
    QemuMutex g_lock;
    bool g_inited;
} BDLogChunk;

/* -------- 내부 유틸 -------- */

static inline LogChunk *bd_log_new_chunk(size_t cap)
{
    LogChunk *c = g_malloc(sizeof(LogChunk) + cap);
    c->next = NULL;
    c->len  = 0;
    c->cap  = cap;
    return c;
}

static void bd_log_init_locked_nolock(BDLogChunk *bdc)
{
    if (!bdc->g_head) {
        bdc->g_head = bd_log_new_chunk(LOG_CHUNK_SIZE);
        bdc->g_tail = bdc->g_head;
        bdc->g_total = 0;
    }
}

static inline void bd_log_ensure_init(BDLogChunk *bdc)
{
    if (G_UNLIKELY(!bdc->g_inited)) {
        qemu_mutex_init(&bdc->g_lock);
        qemu_mutex_lock(&bdc->g_lock);
        bd_log_init_locked_nolock(bdc);
        bdc->g_inited = true;
        qemu_mutex_unlock(&bdc->g_lock);
    }
}

static void bd_log_append_bytes(BDLogChunk *bdc, const char *p, size_t n)
{
    while (n > 0) {
        size_t avail = bdc->g_tail->cap - bdc->g_tail->len;
        if (avail == 0) {
            LogChunk *nc = bd_log_new_chunk(LOG_CHUNK_SIZE);
            bdc->g_tail->next = nc;
            bdc->g_tail = nc;
            avail = bdc->g_tail->cap;
        }
        size_t w = (n < avail) ? n : avail;
        memcpy(bdc->g_tail->data + bdc->g_tail->len, p, w);
        bdc->g_tail->len += w;
        bdc->g_total     += w;
        p += w;
        n -= w;
    }
}

/* 타임스탬프 [YYYY-MM-DD HH:MM:SS] */
static void bd_log_get_timestamp(BDLogChunk *bdc)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld ", ns);
    if (len > 0)
        bd_log_append_bytes(bdc, buf, (size_t)len);
}

/* flush용 스왑: 활성 버퍼를 비우고, 이전 버퍼를 반환 */
static void bd_log_swap_buffers(BDLogChunk *bdc, LogChunk **out_head, size_t *out_total)
{
    *out_head  = bdc->g_head;
    *out_total = bdc->g_total;

    /* 새 활성 버퍼로 교체 */
    bdc->g_head  = bd_log_new_chunk(LOG_CHUNK_SIZE);
    bdc->g_tail  = bdc->g_head;
    bdc->g_total = 0;
}

/* 리스트 free (flush 후 사용) */
static void bd_log_free_list(LogChunk *h)
{
    while (h) {
        LogChunk *n = h->next;
        g_free(h);
        h = n;
    }
}

/* -------- 공개 API -------- */

/* 초기화(첫 사용 시 자동 초기화되므로 선택 사항) */
static inline void bd_log_init(BDLogChunk *bdc)
{
    bd_log_ensure_init(bdc);
}

/* printf 스타일 로그 추가: [ts] + " " + message + "\n" */
static inline void bd_log_add(BDLogChunk *bdc, const char *fmt, ...)
{
    bd_log_ensure_init(bdc);

    qemu_mutex_lock(&bdc->g_lock);

    bd_log_get_timestamp(bdc);

    // 가변 인자 포맷 길이 계산
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);

    if (need < 0) {
        // 포맷 에러 시 최소 메시지
        const char *err = "(format error)";
        bd_log_append_bytes(bdc, err, strlen(err));
    } else {
        // 필요한 길이(+1은 NUL)만큼 임시 버퍼
        char *msg = g_malloc((size_t)need + 1);
        vsnprintf(msg, (size_t)need + 1, fmt, ap);
        bd_log_append_bytes(bdc, msg, (size_t)need);
        g_free(msg);
    }
    va_end(ap);

    bd_log_append_bytes(bdc, "\n", 1);
    qemu_mutex_unlock(&bdc->g_lock);
}

/* flush: 잠깐만 잠그고 버퍼를 스왑 → 잠금 해제 후 파일 기록(긴 I/O 동안 lock 미점유) */
static inline void bd_log_flush_to_file(BDLogChunk *bdc, const char *path)
{
    bd_log_ensure_init(bdc);

    LogChunk *old_head = NULL;
    size_t    old_total = 0;

    qemu_mutex_lock(&bdc->g_lock);
    bd_log_swap_buffers(bdc, &old_head, &old_total);
    qemu_mutex_unlock(&bdc->g_lock);

    if (!old_head)
        return;

    FILE *fp = fopen(path, "wb");  // 항상 새로 생성/덮어쓰기
    if (!fp) {
        fprintf(stderr, "❌ log_flush_to_file: failed to open %s\n", path);
        perror("fopen");
        bd_log_free_list(old_head);
        return;
    }

    size_t total_written = 0;
    for (LogChunk *c = old_head; c; c = c->next) {
        if (c->len == 0) continue;
        size_t w = fwrite(c->data, 1, c->len, fp);
        total_written += w;
        if (w != c->len) {
            fprintf(stderr, "⚠️ partial write on %s (chunk wrote %zu/%zu)\n",
                    path, w, c->len);
            break;
        }
    }
    fclose(fp);

    if (total_written != old_total) {
        fprintf(stderr, "⚠️ log_flush_to_file: wrote %zu/%zu bytes to %s\n",
                total_written, old_total, path);
    } else {
        printf("✅ %zu bytes written to %s (overwritten)\n", total_written, path);
    }

    bd_log_free_list(old_head);
}

/* 내용만 비우고 첫 청크는 재사용(메모리 보존) */
static inline void bd_log_clear(BDLogChunk *bdc)
{
    bd_log_ensure_init(bdc);
    qemu_mutex_lock(&bdc->g_lock);

    LogChunk *old = bdc->g_head;
    /* 새 활성 버퍼로 교체 */
    bdc->g_head  = bd_log_new_chunk(LOG_CHUNK_SIZE);
    bdc->g_tail  = bdc->g_head;
    bdc->g_total = 0;

    qemu_mutex_unlock(&bdc->g_lock);

    bd_log_free_list(old);
}

/* 완전 해제(모든 청크 free) */
static inline void bd_log_free(BDLogChunk *bdc)
{
    if (!bdc->g_inited) return;

    qemu_mutex_lock(&bdc->g_lock);
    LogChunk *old = bdc->g_head;
    bdc->g_head = bdc->g_tail = NULL;
    bdc->g_total = 0;
    qemu_mutex_unlock(&bdc->g_lock);

    bd_log_free_list(old);
}
