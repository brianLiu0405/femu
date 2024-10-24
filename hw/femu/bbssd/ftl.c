#include "../nvme.h"
#include "ftl.h"
#define FEMU_DEBUG_FTL
// #define PRINT_READ_WRITE
// #define RW_DEBUG
#define MODIFY
// #define THREAD
#define THREAD_NUM 4

static void *ftl_thread(void *arg);

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.spp->logic_ttpgs);
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

static struct ppa pgidx2ppa(struct ssd *ssd, uint64_t idx)
{
    struct ppa ppa;
    struct ssdparams *spp = &ssd->sp;

    ppa.g.pg = idx % spp->pgs_per_blk;
    idx /= spp->pgs_per_blk;
    ppa.g.blk = idx % spp->blks_per_pl;
    idx /= spp->blks_per_pl;
    ppa.g.pl = idx % spp->pls_per_lun;
    idx /= spp->pls_per_lun;
    ppa.g.lun = idx % spp->luns_per_ch;
    idx /= spp->luns_per_ch;
    ppa.g.ch = idx;

    uint64_t pgidx = ppa2pgidx(ssd, &ppa);
    ftl_assert(pgidx == idx);

    return ppa;
}


// [Brian] modify
static inline void set_RTTbit(struct ssd *ssd, struct ppa *ppa)
{
    // printf("set RTT bit\r\n");
    uint64_t physical_page_num = ppa2pgidx(ssd, ppa);
    ssd->RTTtbl[physical_page_num] = 1;
}

static inline void clr_RTTbit(struct ssd *ssd, struct ppa *ppa)
{
    // printf("clr RTT bit\r\n");
    uint64_t physical_page_num = ppa2pgidx(ssd, ppa);
    ssd->RTTtbl[physical_page_num] = 0;
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

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
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
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd, void *mb)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    // clean current line
    struct ssdparams *spp = &ssd->sp;
    int bytes_per_pages = spp->secsz * spp->secs_per_pg;
    for(int ch=0; ch<spp->nchs; ch++){
        for(int lun=0; lun<spp->luns_per_ch; lun++){
            for(int pg=0; pg<spp->pgs_per_blk; pg++){
                struct ppa ppa;
                ppa.ppa = 0;
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pg = pg;
                ppa.g.blk = curline->id;
                ppa.g.pl = 0;
                uint64_t physical_page_num = ppa2pgidx(ssd, &ppa);
                memset(((char*)(mb + (physical_page_num * bytes_per_pages))), 0, bytes_per_pages);
                struct FG_OOB *old_OOB = &(ssd->OOB[physical_page_num]);
                old_OOB->LPA = INVALID_LPN;
                old_OOB->P_PPA = INVALID_PPA;
                old_OOB->Timestamp = INVALID_TIME;
                old_OOB->RIP = 0;
            }
        }
    }

    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd, void *mb)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd, mb);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
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

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->logic_ttpgs);
    for (int i = 0; i < spp->logic_ttpgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

// [Brian] modify
static void ssd_init_RTT(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->RTTtbl = g_malloc0(spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->RTTtbl[i] = false;
    }
}

// [Brian] modify
static void ssd_init_OOB(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    // printf("spp->tt_pgs : %d\r\n", spp->tt_pgs);
    ssd->OOB = g_malloc0(OUT_OF_BOND_SPACE_SIZE_PER_PAGE * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->OOB[i].LPA = INVALID_LPN;
        ssd->OOB[i].P_PPA = INVALID_PPA;
        ssd->OOB[i].Timestamp = INVALID_TIME;
        ssd->OOB[i].RIP = 0;
        ssd->OOB[i].rsv = 0;
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

    ssd_init_params(spp, n);


    spp->logic_ttpgs = n->memsz * (1024 / spp->secsz) * (1024 / spp->secs_per_pg);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    // [Brian] modify
    /* initialize RTT */
    ssd_init_RTT(ssd);
    /* initialize OOB */
    ssd_init_OOB(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
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
    return (lpn < ssd->sp.logic_ttpgs);
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
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

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
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
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

static void gc_read_page(struct ssd *ssd, struct ppa *ppa, char* data, void *mb)
{
    struct ssdparams *spp = &ssd->sp;
    int bytes_per_pages = spp->secsz * spp->secs_per_pg;
    uint64_t physical_page_num = ppa2pgidx(ssd, ppa);

    memcpy(data, (char*)(mb + (physical_page_num * bytes_per_pages)), bytes_per_pages);

    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa, char* data, void *mb, int type)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd);

    struct ssdparams *spp = &ssd->sp;
    int bytes_per_pages = spp->secsz * spp->secs_per_pg;
    uint64_t new_physical_page_num = ppa2pgidx(ssd, &new_ppa);
    uint64_t new_start_addr = (uint64_t)mb + (new_physical_page_num * bytes_per_pages);
    
    memcpy((char*)new_start_addr, data, bytes_per_pages);

    // check data correct
    uint64_t old_physical_page_num = ppa2pgidx(ssd, old_ppa);
    uint64_t old_start_addr = (uint64_t)mb + (old_physical_page_num * bytes_per_pages);

    for(int i=0; i<bytes_per_pages; i++){
        if(((char*)new_start_addr)[i] != ((char*)old_start_addr)[i]){
            printf("data move error, data incorrect\r\n");
        }
    }
    // check data correct

    struct FG_OOB *old_OOB = &(ssd->OOB[old_physical_page_num]);
    struct FG_OOB *current_OOB = &(ssd->OOB[new_physical_page_num]);
    if(type){
        // if(lpn != old_OOB->LPA){
        if(lpn != INVALID_LPN){
            printf("RTT bug!\r\n");
            printf("lpn : %lu, old_OOB->LPA : %lu\r\n", lpn, old_OOB->LPA);
            while(1);
        }
        current_OOB->LPA = old_OOB->LPA;
        current_OOB->P_PPA = old_ppa->ppa;
        current_OOB->Timestamp = old_OOB->Timestamp;
        current_OOB->RIP = old_OOB->RIP;
        if(type == SET_RIP) current_OOB->RIP = 1;
        set_rmap_ent(ssd, INVALID_LPN, &new_ppa);
        set_RTTbit(ssd, &new_ppa);
        mark_page_invalid(ssd, &new_ppa);
    }
    else{
        if(lpn == INVALID_LPN){
            printf("normal bug!\r\n");
            printf("lpn : %lu, old_OOB->LPA : %lu\r\n", lpn, old_OOB->LPA);
            while(1);
        }
        current_OOB->LPA = old_OOB->LPA;
        current_OOB->P_PPA = old_ppa->ppa;
        current_OOB->Timestamp = old_OOB->Timestamp;
        current_OOB->RIP = old_OOB->RIP;
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &new_ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &new_ppa);
        mark_page_valid(ssd, &new_ppa);
        // added by brian
        set_rmap_ent(ssd, INVALID_LPN, old_ppa);
        mark_page_invalid(ssd, old_ppa);
        if(ssd->RTTtbl[old_physical_page_num]) set_RTTbit(ssd, &new_ppa);
    }
    clr_RTTbit(ssd, old_ppa);
    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd, mb);

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

static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa, void *mb)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
    int page_size = spp->secsz * spp->secs_per_pg;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            char *data = g_malloc0(page_size);
            gc_read_page(ssd, ppa, data, mb);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa, data, mb, NORMAL);
            g_free(data);
            cnt++;
        }
        else if(pg_iter->status == PG_INVALID){
            uint64_t physical_page_num = ppa2pgidx(ssd, ppa);
            bool curr_RTT = ssd->RTTtbl[physical_page_num];
            if(curr_RTT){
                struct FG_OOB curr_pg_OOB = ssd->OOB[physical_page_num];
                char *data = g_malloc0(page_size);
                gc_read_page(ssd, ppa, data, mb);
                if(curr_pg_OOB.RIP) gc_write_page(ssd, ppa, data, mb, SET_RTT);
                else gc_write_page(ssd, ppa, data, mb, SET_RIP);
                g_free(data);
            }
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int do_gc(struct ssd *ssd, bool force, void* mb)
{
    printf("do_gc\r\n");
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }
    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa, mb);
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

    return 0;
}

int backend_rw_from_flash(SsdDramBackend *b, NvmeRequest *req, uint64_t *lbal, bool is_write, struct ssd *ssd, uint64_t *maxlat)
{
    int sg_cur_index = 0;
    dma_addr_t sg_cur_byte = 0;
    dma_addr_t cur_addr, cur_len;
    uint64_t mb_oft = lbal[0];
    void *mb = b->logical_space;

    DMADirection dir = DMA_DIRECTION_FROM_DEVICE;

    QEMUSGList *qsg = &req->qsg;
    uint64_t lba = mb_oft / 512;
    uint64_t start_lpn = lba / ssd->sp.secs_per_pg;
    uint64_t end_lpn = start_lpn + qsg->nsg - 1;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0;
    uint64_t sublat = 0;
    int r;
    int bytes_per_pages = ssd->sp.secs_per_pg * ssd->sp.secsz;

    if (is_write) {
        dir = DMA_DIRECTION_TO_DEVICE;
        while (should_gc_high(ssd)) {
            /* perform GC here until !should_gc(ssd) */
            r = do_gc(ssd, true, mb);
            if (r == -1)
                break;
        }
    }


    #ifdef RW_DEBUG
    printf("qsg->nsg: %d\r\n", qsg->nsg);
    printf("backend_rw_from_flash: lpn_S %lu, lpn_E %lu\r\n", start_lpn, end_lpn);
    #endif

    for (lpn = start_lpn; lpn <= end_lpn; lpn++){
        #ifdef RW_DEBUG
        printf("logic page number: %lu\r\n", lpn);
        
        #else
        if(sg_cur_index >= qsg->nsg){
            printf("[ERROR] in dram backend rw\r\n");
            break;
        }
        #endif

        cur_addr = qsg->sg[sg_cur_index].base + sg_cur_byte;
        cur_len = qsg->sg[sg_cur_index].len - sg_cur_byte;
        ppa = get_maptbl_ent(ssd, lpn);
        if (is_write) {
            char* rmw_R_buf = g_malloc0(bytes_per_pages);
            char* rmw_W_buf = g_malloc0(bytes_per_pages);
            if (!rmw_R_buf || !rmw_W_buf) {
                if (rmw_R_buf) g_free(rmw_R_buf);
                if (rmw_W_buf) g_free(rmw_W_buf);
                return -1;  // 返回錯誤碼
            }
            struct ppa old_ppa;
            old_ppa.ppa = ppa.ppa;
            
            if (mapped_ppa(&old_ppa)) {
                // read modify write
                uint64_t old_physical_page_num = ppa2pgidx(ssd, &old_ppa);

                memcpy(rmw_R_buf, (char*)(mb + (old_physical_page_num * bytes_per_pages)), bytes_per_pages);
                
                /* update old page information first */
                mark_page_invalid(ssd, &old_ppa);
                set_rmap_ent(ssd, INVALID_LPN, &old_ppa);
            }
            
            ppa = get_new_page(ssd);

            /* update maptbl */
            set_maptbl_ent(ssd, lpn, &ppa);
            /* update rmap */   
            set_rmap_ent(ssd, lpn, &ppa);

            mark_page_valid(ssd, &ppa);

            /* need to advance the write pointer here */
            ssd_advance_write_pointer(ssd, mb);


            // if (dma_memory_rw(qsg->as, cur_addr, mb + (physical_page_num * 4096), cur_len, dir, MEMTXATTRS_UNSPECIFIED)) {
            if (dma_memory_rw(qsg->as, cur_addr, rmw_W_buf, cur_len, dir, MEMTXATTRS_UNSPECIFIED)) {
                femu_err("dma_memory_rw error\n");
            }

            uint64_t lba_off_in_page = (lba % ssd->sp.secs_per_pg) * ssd->sp.secsz;

            memcpy(rmw_R_buf + lba_off_in_page, rmw_W_buf, cur_len);

            uint64_t physical_page_num = ppa2pgidx(ssd, &ppa);

            memcpy((char*)(mb + (physical_page_num * bytes_per_pages)), rmw_R_buf, bytes_per_pages);
            
            struct FG_OOB *current_OOB = &(ssd->OOB[physical_page_num]);
            current_OOB->LPA = lpn;
            current_OOB->P_PPA = old_ppa.ppa;
            current_OOB->Timestamp = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

            struct nand_cmd swr;
            swr.type = USER_IO;
            swr.cmd = NAND_WRITE;
            swr.stime = req->stime;
            /* get latency statistics */
            curlat = ssd_advance_status(ssd, &ppa, &swr);
            *maxlat = (curlat > *maxlat) ? curlat : *maxlat;
            g_free(rmw_R_buf);
            g_free(rmw_W_buf);
        }
        else{
            uint64_t physical_page_num = ppa2pgidx(ssd, &ppa);

            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                char* zero_buf = g_malloc0(bytes_per_pages);
                if (dma_memory_rw(qsg->as, cur_addr, zero_buf, cur_len, dir, MEMTXATTRS_UNSPECIFIED)) {
                    femu_err("dma_memory_rw error\n");
                }
                g_free(zero_buf);
                continue;
            }

            uint64_t lba_off_in_page = (lba % ssd->sp.secs_per_pg) * ssd->sp.secsz;
            if (dma_memory_rw(qsg->as, cur_addr, mb + (physical_page_num * bytes_per_pages) + lba_off_in_page, cur_len, dir, MEMTXATTRS_UNSPECIFIED)) {
                femu_err("dma_memory_rw error\n");
            }

            set_RTTbit(ssd, &ppa);
            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = req->stime;
            sublat = ssd_advance_status(ssd, &ppa, &srd);
            *maxlat = (sublat > *maxlat) ? sublat : *maxlat;
        }

        sg_cur_byte += cur_len;
        if (sg_cur_byte == qsg->sg[sg_cur_index].len) {
            sg_cur_byte = 0;
            ++sg_cur_index;
        }

        if (b->femu_mode == FEMU_OCSSD_MODE) {
            mb_oft = lbal[sg_cur_index];
        } else if (b->femu_mode == FEMU_BBSSD_MODE ||
                   b->femu_mode == FEMU_NOSSD_MODE ||
                   b->femu_mode == FEMU_ZNSSD_MODE) {
            mb_oft += cur_len;
            lba = mb_oft / 512;
        } else {
            assert(0);
        }
    }

    qemu_sglist_destroy(qsg);

    return 0;
}

uint16_t nvme_rw_for_flash(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req, uint64_t *maxlat)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint16_t ctrl = le16_to_cpu(rw->control);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint64_t prp1 = le64_to_cpu(rw->prp1);
    uint64_t prp2 = le64_to_cpu(rw->prp2);
    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint16_t ms = le16_to_cpu(ns->id_ns.lbaf[lba_index].ms);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].lbads;
    uint64_t data_size = (uint64_t)nlb << data_shift;
    uint64_t data_offset = slba << data_shift;
    uint64_t meta_size = nlb * ms;
    uint64_t elba = slba + nlb;
    uint16_t err;
    int ret;

    req->is_write = (rw->opcode == NVME_CMD_WRITE) ? 1 : 0;

    err = femu_nvme_rw_check_req(n, ns, cmd, req, slba, elba, nlb, ctrl,
                                 data_size, meta_size);
    if (err)
        return err;

    if (nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, data_size, n)) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, prp1), 0, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    assert((nlb << data_shift) == req->qsg.size);

    req->slba = slba;
    req->status = NVME_SUCCESS;
    req->nlb = nlb;

    ret = backend_rw_from_flash(n->mbe, req, &data_offset, req->is_write, n->ssd, maxlat);
    if (!ret) {
        return NVME_SUCCESS;
    }

    return NVME_DNR;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req, FemuCtrl *n)
{
    #ifdef PRINT_READ_WRITE
    printf("ssd_read\r\n");
    #endif
    /* new write */
    NvmeNamespace *ns;
    uint32_t nsid = le32_to_cpu(req->cmd.nsid);

    if (nsid == 0 || nsid > n->num_namespaces) {
        femu_err("%s, NVME_INVALID_NSID %" PRIu32 "\n", __func__, nsid);
        return NVME_INVALID_NSID | NVME_DNR;
    }

    req->ns = ns = &n->namespaces[nsid - 1];

    uint16_t status;
    uint64_t maxlat = 0;
    #ifdef MODIFY
        status = nvme_rw_for_flash(n, ns, &req->cmd, req, &maxlat);
    #else
        status = nvme_rw(n, ns, &req->cmd, req);
    #endif

    if (status == NVME_SUCCESS) {
        /* Normal I/Os that don't need delay emulation */
        req->status = status;
    } else {
        femu_err("Error IO processed!\n");
    }

    return maxlat;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req, FemuCtrl *n)
{
    #ifdef PRINT_READ_WRITE
    printf("ssd_write\r\n");
    #endif
    /* new write */
    NvmeNamespace *ns;
    uint32_t nsid = le32_to_cpu(req->cmd.nsid);

    if (nsid == 0 || nsid > n->num_namespaces) {
        femu_err("%s, NVME_INVALID_NSID %" PRIu32 "\n", __func__, nsid);
        return NVME_INVALID_NSID | NVME_DNR;
    }

    req->ns = ns = &n->namespaces[nsid - 1];
    uint64_t maxlat = 0;
    uint16_t status;
    #ifdef MODIFY
        status = nvme_rw_for_flash(n, ns, &req->cmd, req, &maxlat);
    #else
        status = nvme_rw(n, ns, &req->cmd, req);

    #endif
    if (status == NVME_SUCCESS) {
        /* Normal I/Os that don't need delay emulation */
        req->status = status;
    } else {
        femu_err("Error IO processed!\n");
    }

    return maxlat;
}

static uint64_t ssd_secure_erase(struct ssd *ssd, FemuCtrl *n){
    printf("secure_erase \r\n");
    void *mb = n->mbe->logical_space;

    // clean current line
    struct ssdparams *spp = &ssd->sp;
    int bytes_per_pages = spp->secsz * spp->secs_per_pg;
    for(int ch=0; ch<spp->nchs; ch++){
        for(int lun=0; lun<spp->luns_per_ch; lun++){
            for(int pls=0; pls<spp->pls_per_lun; pls++){
                for(int blk=0; blk<spp->blks_per_pl; blk++){
                    for(int pg=0; pg<spp->pgs_per_blk; pg++){
                        struct ppa ppa;
                        ppa.ppa = 0;
                        ppa.g.ch = ch;
                        ppa.g.lun = lun;
                        ppa.g.blk = blk;
                        ppa.g.pl = pls;
                        ppa.g.pg = pg;
                        uint64_t physical_page_num = ppa2pgidx(ssd, &ppa);
                        memset((char*)(mb + (physical_page_num * bytes_per_pages)), 0, bytes_per_pages);
                        struct FG_OOB *old_OOB = &(ssd->OOB[physical_page_num]);
                        old_OOB->LPA = INVALID_LPN;
                        old_OOB->P_PPA = INVALID_PPA;
                        old_OOB->Timestamp = INVALID_TIME;
                        old_OOB->RIP = 0;
                        clr_RTTbit(ssd, &ppa);
                    }
                }
            }
        }
    }
    n->sec_erase = 127;
    return 0;
}

static void swap_in_l2p(struct ssd *ssd, uint64_t phy, uint64_t now_ppa_num, struct ppa iter_ppa, struct ppa now_ppa, char *check, struct nand_page *pg_iter){ 
    if (ssd->OOB[phy].LPA == ssd->OOB[now_ppa_num].LPA) {
        set_rmap_ent(ssd, INVALID_LPN, &now_ppa);
        mark_page_invalid(ssd, &now_ppa);
        set_rmap_ent(ssd, ssd->OOB[phy].LPA, &iter_ppa);
        set_maptbl_ent(ssd, ssd->OOB[phy].LPA, &iter_ppa);
        // mark_page_valid(ssd, &iter_ppa);
        pg_iter->status = PG_FREE;
        check[now_ppa_num] = 1;
    }
}

static uint64_t do_recovery(struct ssd *ssd, FemuCtrl *n){
    printf("recovery \r\n");
    /*     this is recover    */
    struct ssdparams *spp = &ssd->sp;
    char *check = g_malloc0(spp->tt_pgs);
    for (uint64_t phy = 0; phy < spp->tt_pgs; phy++) {
        // printf("phy %lu \r\n", phy);
        struct nand_page *pg_iter = NULL;
        if(!check[phy]){
            struct ppa iter_ppa = pgidx2ppa(ssd, phy);
            pg_iter = get_pg(ssd, &iter_ppa);
            if(ssd->RTTtbl[phy] == 1 && pg_iter->status == PG_INVALID){
                uint64_t lpn_iter_ppa = ssd->OOB[phy].LPA;
                struct ppa now_ppa = get_maptbl_ent(ssd, lpn_iter_ppa);
                uint64_t now_ppa_num = ppa2pgidx(ssd, &now_ppa);
                if(ssd->OOB[phy].RIP){
                    if(!check[now_ppa_num] && ssd->OOB[phy].Timestamp < ssd->OOB[now_ppa_num].Timestamp){
                        swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check, pg_iter);
                    }
                    else if(ssd->OOB[phy].Timestamp > ssd->OOB[now_ppa_num].Timestamp){
                        swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check, pg_iter);
                    }
                }
                else{
                    if(!check[now_ppa_num] && ssd->OOB[phy].Timestamp < ssd->OOB[now_ppa_num].Timestamp){
                        swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check, pg_iter);
                    }
                    else if(ssd->OOB[phy].Timestamp > ssd->OOB[now_ppa_num].Timestamp){
                        swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check, pg_iter);
                    }
                }
            }
        }
        check[phy] = 1;
    }
    g_free(check);

    printf("recovery done\r\n");
    n->sec_erase = 127;
    return 0;
}

static uint64_t do_recovery_new_version(struct ssd *ssd, FemuCtrl *n){
    printf("recovery \r\n");
    uint64_t target_lpa = 3327;
    struct ppa cur_target_ppa = get_maptbl_ent(ssd, target_lpa);
    uint64_t cur_target_ppa_num = ppa2pgidx(ssd, &cur_target_ppa);
    int64_t cur_target_timestamp = ssd->OOB[cur_target_ppa_num].Timestamp; // assume this ransomware attack
    /*     this is recover    */
    struct ssdparams *spp = &ssd->sp;
    char *check = g_malloc0(spp->tt_pgs);
    int64_t original_delta = cur_target_timestamp;
    uint64_t pre_ppa_num = 0;
    for (uint64_t phy = 0; phy < spp->tt_pgs; phy++) {
        if(ssd->OOB[phy].LPA == target_lpa){
            int64_t delta = ssd->OOB[phy].Timestamp - cur_target_timestamp;
            if(delta != 0 && delta < original_delta) {
                original_delta = delta;
                pre_ppa_num = phy;
            }
        }
    }

    int64_t pre_ppa_timestamp = ssd->OOB[pre_ppa_num].Timestamp;
    struct ppa pre_ppa = pgidx2ppa(ssd, pre_ppa_num);
    printf("cur_target_ppa  ch %d, lun %d, pl %d, blk %d, pg %d -> time %ld\r\n", cur_target_ppa.g.ch, cur_target_ppa.g.lun, cur_target_ppa.g.pl, cur_target_ppa.g.blk, cur_target_ppa.g.pg, cur_target_timestamp);
    printf("pre_ppa         ch %d, lun %d, pl %d, blk %d, pg %d -> time %ld\r\n", pre_ppa.g.ch, pre_ppa.g.lun, pre_ppa.g.pl, pre_ppa.g.blk, pre_ppa.g.pg, pre_ppa_timestamp);

//TODO : find current data (encryption) close to ranAttackTime, if timestamp bigger than threshold like average, abandon it, otherwise, swap it
   
    for (uint64_t phy = 0; phy < spp->tt_pgs; phy++) {
        // printf("phy %lu \r\n", phy);
        struct nand_page *pg_iter = NULL;
        if(!check[phy]){
            struct ppa iter_ppa = pgidx2ppa(ssd, phy);
            pg_iter = get_pg(ssd, &iter_ppa);
            if(ssd->RTTtbl[phy] == 1 && pg_iter->status == PG_INVALID){
                uint64_t lpn_iter_ppa = ssd->OOB[phy].LPA;
                struct ppa now_ppa = get_maptbl_ent(ssd, lpn_iter_ppa);
                uint64_t now_ppa_num = ppa2pgidx(ssd, &now_ppa);
                // if(pre_ppa_timestamp > ssd->OOB[phy].Timestamp){
                if(abs(pre_ppa_timestamp - ssd->OOB[phy].Timestamp) < abs(pre_ppa_timestamp - ssd->OOB[now_ppa_num].Timestamp)){
                    printf("swap in l2p, lpa %lu\r\n", lpn_iter_ppa);
                    swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check, pg_iter);
                }
                // }

                // if(ssd->OOB[phy].RIP){
                //     if(!check[now_ppa_num] && ssd->OOB[phy].Timestamp < ssd->OOB[now_ppa_num].Timestamp){
                //         swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check, pg_iter);
                //     }
                //     else if(ssd->OOB[phy].Timestamp > ssd->OOB[now_ppa_num].Timestamp){
                //         swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check, pg_iter);
                //     }
                // }
                // else{
                //     if(!check[now_ppa_num] && ssd->OOB[phy].Timestamp < ssd->OOB[now_ppa_num].Timestamp){
                //         swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check, pg_iter);
                //     }
                //     else if(ssd->OOB[phy].Timestamp > ssd->OOB[now_ppa_num].Timestamp){
                //         swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check, pg_iter);
                //     }
                // }
            }
        }
        check[phy] = 1;
    }
    g_free(check);

    printf("recovery done\r\n");
    n->sec_erase = 127;
    return 0;
}

struct SsdMbPackage {
    FemuCtrl *n;
    struct ssd *ssd;
    char *mb;
    int id;
};

static void *worker(void *arg)
{
    struct SsdMbPackage *pkg = (struct SsdMbPackage *)arg;
    FemuCtrl *n = pkg->n;
    struct ssd *ssd = pkg->ssd;
    char *mb = pkg->mb;
    int id = pkg->id;
    int bytes_per_pages = ssd->sp.secs_per_pg * ssd->sp.secsz;
    // printf("id %d \r\n", id);
    for (uint64_t physical_page_num = id; physical_page_num < ssd->sp.tt_pgs; physical_page_num += 4) {
        // printf("physical_page_num %lu \r\n", physical_page_num);
        struct ppa ppa = pgidx2ppa(ssd, physical_page_num);

        char file_name[255];
        sprintf(file_name, "mySSD/ch%d/lun%d/pl%d/blk%d/pg%d", ppa.g.ch, ppa.g.lun, ppa.g.pl, ppa.g.blk, ppa.g.pg);  // Use .bin for binary files
        int fd = open(file_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            perror("file open fail");
        }
        
        uint64_t writeOOB = write(fd, &ssd->OOB[physical_page_num], sizeof(struct FG_OOB));
        if (writeOOB != (uint64_t)sizeof(struct FG_OOB)) {
            printf("written %lu \r\n", (uint64_t)writeOOB);
            perror("file write fail");
            close(fd);  // Close the file descriptor
            n->sec_erase = 127;
        }
        // Calculate the offset for the current page
        char *ram_data = mb + (physical_page_num * bytes_per_pages);
        uint64_t written = write(fd, ram_data, bytes_per_pages);
        if (written != (uint64_t)bytes_per_pages) {
            printf("written %lu \r\n", (uint64_t)written);
            perror("file write fail");
            close(fd);  // Close the file descriptor
            n->sec_erase = 127;
        }

        close(fd);  // Close the file descriptor after writing
    }
}

int p2l_file_num = 0;
static uint64_t dump_p2l(struct ssd *ssd, FemuCtrl *n){
    printf("dump p2l\r\n");
    struct ssdparams *spp = &ssd->sp;

    char file_name[32];
    sprintf(file_name, "mySSD/L2P_%d", p2l_file_num++);  // Use .bin for binary files
    FILE *file = fopen(file_name, "wb");  // Open the file in binary write mode
    if (!file) {
        perror("file open fail");
        return 1;
    }
    for (int logic = 0; logic < spp->logic_ttpgs; logic++) {
        struct ppa ppa = get_maptbl_ent(ssd, logic);
        if (ppa.ppa != INVALID_PPA) {
            fprintf(file, "logic %d: ch %d, lun %d, plane %d, block %d, page %d, timestamp %lu\n", 
                    logic, ppa.g.ch, ppa.g.lun, ppa.g.pl, ppa.g.blk, ppa.g.pg, ssd->OOB[ppa2pgidx(ssd, &ppa)].Timestamp);
        }
        else{
            fprintf(file, "logic %d: physical %d\n", logic, -1);
        }
    }
    fclose(file);
    n->sec_erase = 127;

    
    for (uint64_t phy = 0; phy < spp->tt_pgs; phy++) {
        if(ssd->OOB[phy].LPA == 3327){
            struct ppa twofivesix = pgidx2ppa(ssd, phy);
            printf("ch %d, lun %d, plane %d, block %d, page %d -> time %ld\r\n", twofivesix.g.ch, twofivesix.g.lun, twofivesix.g.pl, twofivesix.g.blk, twofivesix.g.pg, ssd->OOB[phy].Timestamp);
        }
    }

    return 0;
}

static uint64_t dump(struct ssd *ssd, FemuCtrl *n){
    printf("dump disk\r\n");

    char *mb = (char*) n->mbe->logical_space;
    struct ssdparams *spp = &ssd->sp;

    char file_name[32];
    sprintf(file_name, "mySSD/L2P");  // Use .bin for binary files
    FILE *file = fopen(file_name, "wb");  // Open the file in binary write mode
    if (!file) {
        perror("file open fail");
        return 1;
    }
    for (int logic = 0; logic < spp->logic_ttpgs; logic++) {
        struct ppa ppa = get_maptbl_ent(ssd, logic);
        if (ppa.ppa != INVALID_PPA) {
            fprintf(file, "logic %d: ch %d, lun %d, plane %d, block %d, page %d, timestamp %lu\n", 
                    logic, ppa.g.ch, ppa.g.lun, ppa.g.pl, ppa.g.blk, ppa.g.pg, ssd->OOB[ppa2pgidx(ssd, &ppa)].Timestamp);
        }
        else{
            fprintf(file, "logic %d: physical %d\n", logic, -1);
        }
    }
    fclose(file);
    
    QemuThread threads[THREAD_NUM];
    struct SsdMbPackage thread_ids[THREAD_NUM];

     for (int i = 0; i < THREAD_NUM; i++) {
        thread_ids[i].id = i;
        thread_ids[i].mb = mb;
        thread_ids[i].ssd = ssd;
        thread_ids[i].n = n;
        qemu_thread_create(&threads[i], "worker", worker, &thread_ids[i], QEMU_THREAD_JOINABLE);
    }

    // 等待所有執行緒完成
    for (int i = 0; i < THREAD_NUM; i++) {
        qemu_thread_join(&threads[i]);
    }
    
    printf("\r\nwrite file successful\r\n");
    
    n->sec_erase = 127;
    return 0;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    // ctrl = n;
    // printf("in ftl.c dma address 0x%p\n", n->mbe);
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
        if(n->sec_erase == 0) dump_p2l(ssd, n);
        if(n->sec_erase == 1) ssd_secure_erase(ssd, n);
        else if(n->sec_erase == 2) {
            // do_recovery(ssd, n);
            do_recovery_new_version(ssd, n);
        }
        else if(n->sec_erase == 3) dump(ssd, n);
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                #ifdef THREAD
                printf("\nSSD #%ld\r\n", pthread_self());
                #endif
                lat = ssd_write(ssd, req, n);
                break;
            case NVME_CMD_READ:
                #ifdef THREAD
                printf("\nSSD #%ld\r\n", pthread_self());
                #endif
                lat = ssd_read(ssd, req, n);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false, n->mbe->logical_space);
            }
        }
    }

    return NULL;
}
