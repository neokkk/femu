#include "ftl.h"

//#define FEMU_DEBUG_FTL

static void *ftl_thread(void *arg);

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->rm.free_ru_cnt <= ssd->fsp.gc_thres_rus);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->rm.free_ru_cnt <= ssd->fsp.gc_thres_rus_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);
    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);
    ssd->rmap[pgidx] = lpn;
}

static inline int victim_ru_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_ru_get_pri(void *a)
{
    return ((struct ru *)a)->tt_vpc;
}

static inline void victim_ru_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct ru *)a)->tt_vpc = pri;
}

static inline size_t victim_ru_get_pos(void *a)
{
    return ((struct ru *)a)->pos;
}

static inline void victim_ru_set_pos(void *a, size_t pos)
{
    ((struct ru *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    // lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
    //         victim_line_get_pri, victim_line_set_pri,
    //         victim_line_get_pos, victim_line_set_pos);
    // QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->ru = NULL;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    // lm->victim_line_cnt = 0;
    // lm->full_line_cnt = 0;

    printf("[FEMU] ssd_init_lines; tt_lines: %d, line size: %lu\n",
           lm->tt_lines, (uint64_t)spp->secsz * spp->tt_secs / lm->tt_lines);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *cur_line = NULL;
   
    cur_line = QTAILQ_FIRST(&lm->free_line_list);
    if (!cur_line) {
        printf("[FEMU] no free lines left\n");
        return NULL;
    }

    printf("[FEMU] get_next_free_line; new line: %d, free_line_cnt: %d\n",
           cur_line->id, lm->free_line_cnt);

    QTAILQ_REMOVE(&lm->free_line_list, cur_line, entry);
    lm->free_line_cnt--;

    return cur_line;
}

static void ssd_init_rus(struct ssd *ssd)
{
    struct fdp_ssdparams *fspp = &ssd->fsp;
    struct ru_mgmt *rm = &ssd->rm;
    
    rm->tt_rus = fspp->tt_rus;
    rm->rus = g_malloc0(sizeof(struct ru) * rm->tt_rus);

    QTAILQ_INIT(&rm->free_ru_list);
    rm->victim_ru_pq = pqueue_init(rm->tt_rus, victim_ru_cmp_pri,
                                   victim_ru_get_pri, victim_ru_set_pri,
                                   victim_ru_get_pos, victim_ru_set_pos);
    QTAILQ_INIT(&rm->full_ru_list);
    
    rm->free_ru_cnt = 0;
    for (int i = 0; i < rm->tt_rus; i++) {
        struct ru *ru = &rm->rus[i];
        ru->id = i;
        ru->rg_id = i / fspp->rus_per_rg;
        ru->lines = g_malloc0(sizeof(struct line *) * fspp->lines_per_ru);
        
        // Line 할당
        for (int j = 0; j < fspp->lines_per_ru; j++) {
            ru->lines[j] = NULL;
        }
        ru->cur_line_idx = 0;
        
        QTAILQ_INSERT_TAIL(&rm->free_ru_list, ru, entry);
        rm->free_ru_cnt++;
    }

    ftl_assert(rm->free_ru_cnt == rm->tt_rus);
    rm->victim_ru_cnt = 0;
    rm->full_ru_cnt = 0;

    printf("[FEMU] ssd_init_rus; tt_rus: %d\n", rm->tt_rus);
}

static struct ru *get_next_free_ru(struct ssd *ssd)
{
    struct fdp_ssdparams *fspp = &ssd->fsp;
    struct ru_mgmt *rm = &ssd->rm;
    struct ru *cur_ru = NULL;
   
    cur_ru = QTAILQ_FIRST(&rm->free_ru_list);
    if (!cur_ru) {
        printf("[FEMU] No free rus left\n");
        return NULL;
    }

    printf("[FEMU] get_next_free_ru; new ru: %d, free_ru_cnt: %d, victim_ru_cnt: %d, full_ru_cnt: %d, total_cnt: %d\n",
           cur_ru->id, rm->free_ru_cnt, rm->victim_ru_cnt, rm->full_ru_cnt,
           rm->free_ru_cnt + rm->victim_ru_cnt + rm->full_ru_cnt);

    QTAILQ_REMOVE(&rm->free_ru_list, cur_ru, entry);
    rm->free_ru_cnt--;

    for (int i = 0; i < fspp->lines_per_ru; i++) {
        cur_ru->lines[i] = get_next_free_line(ssd);
        if (!cur_ru->lines[i]) {
            fprintf(stderr, "No free line for RU!\n");
            return NULL;
        }
        cur_ru->lines[i]->ru = cur_ru;
    }

    cur_ru->cur_line_idx = 0;
    cur_ru->tt_vpc = 0;
    cur_ru->tt_ipc = 0;

    return cur_ru;
}

static void ssd_init_write_pointer(struct ssd *ssd, struct write_pointer *wpp)
{
    struct ssdparams *spp = &ssd->sp;
    struct ru *cur_ru = NULL;
    struct line *cur_line = NULL;

    cur_ru = get_next_free_ru(ssd);
    cur_line = cur_ru->lines[cur_ru->cur_line_idx];

    /* wpp->cur_line is always our next-to-write super-block */
    wpp->cur_ru = cur_ru;
    wpp->cur_line = cur_line;
    wpp->ch = wpp->start_ch = 0;
    wpp->lun = wpp->start_lun = 0;
    // wpp->ch = wpp->start_ch = ssd->cur_offset % spp->nchs;
    // wpp->lun = wpp->start_lun = (ssd->cur_offset / spp->nchs) % spp->luns_per_ch;
    wpp->pg = 0;
    wpp->blk = cur_line->id;
    wpp->pl = 0;
    wpp->pos_in_line = 0;

    ssd->cur_offset = (ssd->cur_offset + 1) % (spp->nchs * spp->luns_per_ch);

    printf("[FEMU] ssd_init_write_pointer; ru %d, line %d, start_ch: %d, start_lun: %d\n",
           cur_ru->id, cur_line->id, wpp->start_ch, wpp->start_lun);
}

static void ssd_init_ruhs(struct ssd *ssd)
{
    struct fdp_ssdparams *fspp = &ssd->fsp;
    struct ru_handle *ruh;
    struct write_pointer *gc_shared_wpp = NULL;
    struct ru *ru = NULL;

    printf("[FEMU] ssd_init_ruhs; nruh: %d\n", fspp->nruh);
    // objt_store_init(&ssd->trace_store, fspp->tt_rus);

    ssd->cur_offset = 0;
    ssd->ruhs = g_malloc0(sizeof(struct ru_handle) * fspp->nruh);
    ssd->gc_wpps = g_malloc0(sizeof(struct write_pointer *) * fspp->nruh);

    for (int i = 0; i < fspp->nruh; i++) {
        ruh = &ssd->ruhs[i];
        ruh->id = i;
        ruh->ruamw = 0;
        ruh->ruht = fspp->ruh_types[i];

        ruh->wps = g_malloc0(sizeof(struct write_pointer) * fspp->nrg);
        for (int j = 0; j < fspp->nrg; j++) {
            ssd_init_write_pointer(ssd, &ruh->wps[j]);
            ru = ruh->wps[j].cur_ru;
            ru->ruh = ruh;
            ru->gc = false;
            // objt_on_create(&ssd->trace_store, line->id, ruh->id, false);
        }

        //> nk: for GC
        if (ruh->ruht == NVME_RUHT_INITIALLY_ISOLATED) {
            if (!gc_shared_wpp) {
                gc_shared_wpp = g_malloc0(sizeof(struct write_pointer));
                if (!gc_shared_wpp) {
                    printf("Fail to allocate shared gc wp\n");
                }

                ssd_init_write_pointer(ssd, gc_shared_wpp);
                ru = gc_shared_wpp->cur_ru;
                ru->ruh = ruh;
                ru->gc = true;
                // objt_on_create(&ssd->trace_store, line->id, ruh->id, true);
            }
            ssd->gc_wpps[i] = gc_shared_wpp;
        } else if (ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
            ssd->gc_wpps[i] = g_malloc0(sizeof(struct write_pointer));
            ssd_init_write_pointer(ssd, ssd->gc_wpps[i]);
            ru = ssd->gc_wpps[i]->cur_ru;
            ru->ruh = ruh;
            ru->gc = true;
            // objt_on_create(&ssd->trace_store, line->id, ruh->id, true);
        } else {
            fprintf(stderr, "Cannot reach here\n");
            abort();
        }
        ruh->gc_wpp = ssd->gc_wpps[i];
        printf("ruh %d gc_wpp addr: 0x%lx\n", i, (uintptr_t)ssd->gc_wpps[i]);
    }
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

// 0: normal, 1: line chagned, 2: ru changed
static int ssd_advance_write_pointer(struct ssd *ssd, struct write_pointer *wpp)
{
    struct ssdparams *spp = &ssd->sp;
    struct fdp_ssdparams *fspp = &ssd->fsp;
    struct ru_mgmt *rm = &ssd->rm;
    struct ru *cur_ru = wpp->cur_ru;
    int ret = 0;

    const uint64_t NCHS   = (uint64_t)spp->nchs;
    const uint64_t NLUNS  = (uint64_t)spp->luns_per_ch;
    const uint64_t NPGS   = (uint64_t)spp->pgs_per_blk;
    const uint64_t SUPER  = NCHS * NLUNS;         // 한 페이지(=superpage)당 채널×웨이 포지션 수
    const uint64_t TOTAL_POS_LINE = SUPER * NPGS;      // 라인 전체 포지션 수

    // printf("[FEMU] ssd_advance_write_pointer; line: %d, ruhid: %d, gc: %d, ch: %d, lun: %d, blk: %d, pg: %d\n",
    //        wpp->cur_line->id, wpp->cur_line->ruh->id, wpp->cur_line->gc, wpp->ch, wpp->lun, wpp->blk, wpp->pg);

    check_addr(wpp->ch, spp->nchs);
    wpp->pos_in_line++;

    if (wpp->pos_in_line == TOTAL_POS_LINE) {
        // ---- 라인 종료: 라인 상태 이동 및 새 라인 선택 ----
        wpp->pos_in_line = 0;
        cur_ru->cur_line_idx++;

        if (cur_ru->cur_line_idx >= fspp->lines_per_ru) {
            if (cur_ru->tt_vpc == fspp->pgs_per_ru) {
                ftl_assert(cur_ru->tt_ipc == 0);
                QTAILQ_INSERT_TAIL(&rm->full_ru_list, cur_ru, entry);
                rm->full_ru_cnt++;
            } else {
                ftl_assert(cur_ru->tt_vpc >= 0 && cur_ru->tt_vpc < fspp->pgs_per_ru);
                ftl_assert(wpp->cur_line->ipc > 0);
                pqueue_insert(rm->victim_ru_pq, cur_ru);
                rm->victim_ru_cnt++;
            }

            // 새 RU 선택
            wpp->cur_ru = get_next_free_ru(ssd);
            if (!wpp->cur_ru) {
                fprintf(stderr, "No free ru available!\n");
                abort();
                return 0;
            }

            cur_ru = wpp->cur_ru;
            ret = 2;
        }

        wpp->cur_line = cur_ru->lines[cur_ru->cur_line_idx];
        wpp->blk = wpp->cur_line->id;
        wpp->pg  = 0;
        wpp->pl  = 0; // plane 1개 가정
        wpp->ch  = wpp->start_ch;
        wpp->lun = wpp->start_lun;
        
        // printf("[FEMU] ssd_advance_write_pointer; new line %d, start_ch: %d, start_lun: %d\n",
        //        wpp->cur_line->id, wpp->start_ch, wpp->start_lun);

        return ret;
    }

    // ---- 현재 pos_in_line을 (ch, lun, pg)로 복원 ----
    uint64_t idx     = wpp->pos_in_line;       // 1..TOTAL_POS-1 범위
    uint64_t pg      = idx / SUPER;            // 0..NPGS-1
    uint64_t within  = idx % SUPER;            // 0..SUPER-1

    uint64_t lun_off = within / NCHS;        // 0..NLUNS-1
    uint64_t ch_off  = within % NCHS;        // 0..NCHS-1

    wpp->pg  = (int)pg;
    wpp->ch  = (wpp->start_ch  + (int)ch_off)  % spp->nchs;
    wpp->lun = (wpp->start_lun + (int)lun_off) % spp->luns_per_ch;

    return 0; // 같은 라인 계속 사용
}

static struct ppa get_new_page(struct ssd *ssd, struct write_pointer *wpp)
{
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);
    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n)
{
    spp->secsz = n->bb_params.secsz; // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk; //256
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun; // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch; // 8
    spp->nchs = n->bb_params.nchs; // 8

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;

    check_params(spp);
}

static void ssd_init_fdp_params(struct ssd *ssd, FemuCtrl *n)
{
    struct ssdparams *spp = &ssd->sp;
    struct fdp_ssdparams *fspp = &ssd->fsp;
    struct NvmeEnduranceGroup *endgrp = ssd->endgrp;

    fspp->nrg = n->fdp_params.nrg;
    fspp->nruh = n->fdp_params.nruh;
    fspp->runs = n->fdp_params.runs;

    uint64_t line_sz = (uint64_t)spp->pgs_per_line * (spp->secs_per_pg * spp->secsz);
    printf("line_sz: %ld\n", line_sz);
    ftl_assert(fspp->runs >= line_sz);
    ftl_assert(fspp->runs % line_sz == 0);

    fspp->tt_rus = (uint64_t)spp->secsz * spp->tt_secs / fspp->runs;
    fspp->rus_per_rg = fspp->tt_rus / fspp->nrg;
    fspp->lines_per_ru = fspp->runs / line_sz;
    fspp->pgs_per_ru = spp->pgs_per_line * fspp->lines_per_ru;

    fspp->gc_thres_rus = spp->gc_thres_lines / fspp->lines_per_ru;
    fspp->gc_thres_rus_high = spp->gc_thres_lines_high / fspp->lines_per_ru;

    fspp->lbafi = NVME_ID_NS_FLBAS_INDEX(ssd->ns->id_ns.flbas);
    fspp->ruamw = endgrp->fdp.runs >> ssd->ns->id_ns.lbaf->lbads;

    fspp->ruh_types = (uint8_t *)malloc(sizeof(uint8_t) * n->fdp_params.nruh);
    for (int i = 0; i < n->fdp_params.nruh; i++) {
        fspp->ruh_types[i] = endgrp->fdp.ruhs[i].ruht;
    }

    printf("[FEMU] ssd_init_fdp_params; nruh: %d, tt_sz: %lu, tt_rus: %d, pgs_per_ru: %d,\n"
           "lines_per_ru: %d, ruamw: %lu, line_sz: %lu, gc_thres_rus: %d, gc_thres_rus_high: %d\n",
           fspp->nruh, (uint64_t)spp->tt_secs * spp->secsz, fspp->tt_rus,fspp->pgs_per_ru,
           fspp->lines_per_ru, fspp->ruamw, line_sz, fspp->gc_thres_rus, fspp->gc_thres_rus_high);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);

    ssd->gc_count = 0;
    ssd->endgrp = &n->endgrp;
    ssd->ns = &n->namespaces[0];

    ssd_init_params(spp, n);
    ssd_init_fdp_params(ssd, n);

    n->write_trace_fp = fopen("write_trace.log", "w");
    if (!n->write_trace_fp) {
        printf("Fail to open for write trace log\n");
        exit(-1);
    }

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    ssd_init_rus(ssd);

    ssd_init_ruhs(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */

    qemu_thread_create(&ssd->ftl_thread, "femu-ftl-thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

void ssd_log(FemuCtrl *n)
{
    // FILE *fp;
    printf("ssd_log\n");
    // fp = fopen("log.csv", "w");
    // if (fp == NULL) {
    //     perror("Fail to open file");
    //     return;
    // }
    // objt_dump_csv(&n->ssd->trace_store, fp);
    // fclose(fp);

    // fwrite("\n", 1, 1, n->write_trace_fp);
    // fflush(n->write_trace_fp);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) : ncmd->stime;
    uint64_t nand_stime, chnl_stime;
    struct ssdparams *spp = &ssd->sp;
    struct ssd_channel *ch = get_ch(ssd, ppa);
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
// #if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
// #endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
#if 0
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;
#endif

// #if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
// #endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        printf("[FEMU] unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct ru_mgmt *rm = &ssd->rm;
    struct fdp_ssdparams *fspp = &ssd->fsp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_ru = false;
    struct line *line;
    struct ru *ru;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);

    ru = line->ru;
    if (ru->tt_vpc == fspp->pgs_per_ru) {
        ftl_assert(ru->tt_ipc == 0);
        was_full_ru = true;
    }

    line->ipc++;
    ru->tt_ipc++;

    ftl_assert(ru->tt_vpc > 0 && ru->tt_vpc <= fspp->pgs_per_ru);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (ru->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(rm->victim_ru_pq, ru->tt_vpc - 1, ru);
    } else {
        ru->tt_vpc--;
        line->vpc--;
    }

    if (was_full_ru) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&rm->full_ru_list, ru, entry);
        rm->full_ru_cnt--;
        pqueue_insert(rm->victim_ru_pq, ru);
        rm->victim_ru_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
    line->ru->tt_vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

static inline struct ru_handle *get_ruh(struct ssd *ssd, struct ppa *ppa)
{
    struct line *line = get_line(ssd, ppa);
    return line->ru->ruh;
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    struct ru_handle *ruh = get_ruh(ssd, old_ppa);
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    struct ru *new_ru = NULL;
    int ret;

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd, ruh->gc_wpp);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    nvme_fdp_stat_inc(&ssd->endgrp->fdp.mbmw, 1);

    /* need to advance the write pointer here */
    ret = ssd_advance_write_pointer(ssd, ruh->gc_wpp);

    if (ret == 2) { // allocate new ru
        new_ru = ruh->gc_wpp->cur_ru;
        new_ru->ruh = ruh;
        new_ru->gc = true;
        // objt_on_create(&ssd->trace_store, new_line->id, ruh->id, true);
        // objt_on_full(&ssd->trace_store, prev_line->id);
    }

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct ru *select_victim_ru(struct ssd *ssd, bool force)
{
    struct ru_mgmt *rm = &ssd->rm;
    struct ru *victim_ru = NULL;

    victim_ru = pqueue_peek(rm->victim_ru_pq);
    if (!victim_ru) {
        return NULL;
    }

    if (!force && victim_ru->tt_ipc < ssd->sp.pgs_per_line) {
        return NULL;
    }

    pqueue_pop(rm->victim_ru_pq);
    victim_ru->pos = 0;
    rm->victim_ru_cnt--;

    /* victim_line is a danggling node now */
    return victim_ru;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    struct ru *ru = line->ru;
    int ipc = line->ipc;
    int vpc = line->vpc;
    // objt_on_reclaim(&ssd->trace_store, line->id, line->vpc);
    line->ipc = 0;
    line->vpc = 0;
    line->ru = NULL;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
    ru->tt_ipc -= ipc;
    ru->tt_vpc -= vpc;
}

static int do_gc(struct ssd *ssd, bool force, FemuCtrl *n)
{
    struct ssdparams *spp = &ssd->sp;
    struct fdp_ssdparams *fspp = &ssd->fsp;
    struct ru_mgmt *rm = &ssd->rm;
    struct ru *victim_ru = NULL;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_ru = select_victim_ru(ssd, force);
    if (!victim_ru) {
        return -1;
    }

    printf("do_gc (force: %d); ru: %d (gc: %d), ruhid: %d, count: %d, vpc: %d, ipc: %d\n",
           force, victim_ru->id, victim_ru->gc, victim_ru->ruh->id, ++ssd->gc_count, victim_ru->tt_vpc, victim_ru->tt_ipc);

    for (int i = 0; i < fspp->lines_per_ru; i++) {
        ppa.g.blk = victim_ru->lines[i]->id;
        // write_trace(n->write_trace_fp, "%lu [start] do_gc; force: %d, line: %d, gc: %d, ruhid: %d\n",
        //             nsec_now_mono(), force, victim_line->id, victim_line->gc, victim_line->ruh->id);
        
        /* copy back valid data */
        for (ch = 0; ch < spp->nchs; ch++) {
            for (lun = 0; lun < spp->luns_per_ch; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                lunp = get_lun(ssd, &ppa);
                clean_one_block(ssd, &ppa);
                mark_block_free(ssd, &ppa);

                if (spp->enable_gc_delay) {
                    struct nand_cmd gce;
                    gce.type = GC_IO;
                    gce.cmd = NAND_ERASE;
                    gce.stime = 0;
                    ssd_advance_status(ssd, &ppa, &gce);
                }

                lunp->gc_endtime = lunp->next_lun_avail_time;
            }
        }

        /* update line status */
        mark_line_free(ssd, &ppa);
        // write_trace(n->write_trace_fp, "%lu [end] do_gc\n", nsec_now_mono());
    }

    ftl_assert(victim_ru->tt_vpc == 0 && victim_ru->tt_ipc == 0);
    for (int i = 0; i < fspp->lines_per_ru; i++) {
        victim_ru->lines[i] = NULL;  // line 참조 해제
    }
    victim_ru->cur_line_idx = 0;
    victim_ru->ruh = NULL;
    victim_ru->gc = false;

    QTAILQ_INSERT_TAIL(&rm->free_ru_list, victim_ru, entry);
    rm->free_ru_cnt++;

    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        printf("[FEMU] ssd_read; start_lpn: %"PRIu64", tt_pgs: %d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static int count = 0;
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req, FemuCtrl *n)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    NvmeNamespace *ns = req->ns;
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    // uint32_t dw12 = le32_to_cpu(req->cmd.cdw12);
    // uint8_t dtype = (dw12 >> 20) & 0xf;
    uint16_t pid = le16_to_cpu((rw->dsmgmt >> 16) & 0xffff);
    uint16_t phid, rgid, ruhid;
    struct ru_handle *ruh;
    struct write_pointer *wp;
    struct ru *new_ru = NULL;
    int ret;

    if (end_lpn >= spp->tt_pgs) {
        printf("[FEMU] ssd_write; start_lpn: %"PRIu64", tt_pgs: %d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true, n);
        if (r == -1)
            break;
    }

    // if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
    if (!nvme_parse_pid(ns, pid, &phid, &rgid)) {
        printf("fail to parse pid\n");
        phid = 0;
        rgid = 0;
    }

    ruhid = ns->fdp.phs[phid];
    ruh = &ssd->ruhs[ruhid];
    wp = &ruh->wps[rgid];

    // write_trace(n->write_trace_fp, "%lu [start] ssd_write; lba: %ld, ruhid: %d\n", nsec_now_mono(), start_lpn, ruhid);
    if (++count < 100)
      printf("[FEMU] ssd_write; slba: %"PRIu64", nlb: %d, phid: %d, rgid: %d\n", lba, len, phid, rgid);

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(ssd, wp);

        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        nvme_fdp_stat_inc(&ns->endgrp->fdp.hbmw, 1);
        nvme_fdp_stat_inc(&ns->endgrp->fdp.mbmw, 1);

        /* need to advance the write pointer here */
        // struct line *prev_line = wp->cur_line;
        ret = ssd_advance_write_pointer(ssd, wp);

        if (ret == 2) { //> allocate new RU
            new_ru = wp->cur_ru;
            new_ru->ruh = ruh;
            new_ru->gc = false;
            // objt_on_create(&ssd->trace_store, new_line->id, ruh->id, false);
            // objt_on_full(&ssd->trace_store, prev_line->id);
        }

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;

        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    // write_trace(n->write_trace_fp, "%lu [end] ssd_write\n", nsec_now_mono());

    if (count < 100)
      printf("%ld\n", maxlat);
    return maxlat;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("[FEMU] to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req, n);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                //printf("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                printf("[FEMU] to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false, n);
            }
        }
    }

    fclose(n->write_trace_fp);
    return NULL;
}
