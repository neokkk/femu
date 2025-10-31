// obj_trace_store.h — QEMU/GLib 가정
#pragma once
#include "qemu/osdep.h"
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
static inline bool objt_on_create(ObjTraceStore *st, int64_t id, bool gc)
{
    g_return_val_if_fail(st && st->accum && st->active, false);
    if (g_hash_table_contains(st->active, &id)) return false; // 활성 충돌
    obj_trace_t tr = {
        .id = id,
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
        "id,copied_pgs,t_create_ns,t_full_ns,t_reclaim_ns,t_wall_create_ns,"
        "create_to_full_ms,full_to_reclaim_ms,lifecycle_ms,gc\n");
    for (guint i = 0; i < st->accum->len; i++) {
        const obj_trace_t *tr = &g_array_index(st->accum, obj_trace_t, i);
        double c2f_ms = (tr->t_full_ns     ? (double)(tr->t_full_ns     - tr->t_create_ns)/1e6 : 0.0);
        double f2r_ms = (tr->t_full_ns && tr->t_reclaim_ns) ? (double)(tr->t_reclaim_ns - tr->t_full_ns)/1e6 : 0.0;
        double life_ms= (tr->t_reclaim_ns  ? (double)(tr->t_reclaim_ns  - tr->t_create_ns)/1e6 : 0.0);
        fprintf(fp, "%" PRId64 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%.3f,%.3f,%d\n",
                tr->id, tr->valid_pgs, tr->t_create_ns, tr->t_full_ns, tr->t_reclaim_ns, tr->t_wall_create_ns,
                c2f_ms, f2r_ms, life_ms, tr->gc);
    }
}

