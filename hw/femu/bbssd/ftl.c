#include "../nvme.h"
#include "ftl.h"
#include <math.h>
// #define PRINT_READ_WRITE
// #define RW_DEBUG
#define MODIFY
// #define THREAD
#define THREAD_NUM 4
// #define GC_DEBUG
#define ENTROPY
#define ENTROPY_INCREASE_THRESHOLD 1.2

uint64_t GC_count = 0;
uint64_t backup_pages = 0;
uint64_t write_count = 0;
uint64_t write_in_ssd_count = 0;
uint64_t file_flag = 0x8000000000000000;

static struct line *get_older_block(struct ssd *ssd, void *mb);
static struct line *get_young_block(struct ssd *ssd, void *mb);
uint64_t calculate_hamming_distance(struct ssd *ssd, uint64_t *data1, uint64_t *data2, uint64_t offset, uint64_t length);
static float calculate_entropy(unsigned char *for_hamming_distance, unsigned char *rmw_R_buf, uint64_t lba_off_in_page, uint64_t cur_len);
static void update_to_new(struct ssd *ssd, struct ppa *build_ppa, uint64_t logic, void *mb);
static void init_backup_wp(struct ssd *ssd, void *mb);
static struct ppa write_to_backup_block(struct ssd *ssd, struct ppa *ppa, void *mb);

static void print_line_status(struct ssd *ssd){
    struct line *Oline = NULL;
    printf("free line list \r\n");
    QTAILQ_FOREACH(Oline, &ssd->lm.free_line_list, entry){
        printf("line->id %d, line->vpc %d, line->ipc %d \r\n", Oline->id, Oline->vpc, Oline->ipc);
    }
    Oline = NULL;
    printf("full line list \r\n");
    QTAILQ_FOREACH(Oline, &ssd->lm.full_line_list, entry){
        printf("line->id %d, line->vpc %d, line->ipc %d \r\n", Oline->id, Oline->vpc, Oline->ipc);
        // printf("line->id %d \r\n", Oline->id);
    }
    Oline = NULL;
    printf("sec backup list \r\n");
    QTAILQ_FOREACH(Oline, &ssd->lm.backup_list, entry){
        printf("line->id %d, line->vpc %d, line->ipc %d \r\n", Oline->id, Oline->vpc, Oline->ipc);
        // printf("line->id %d \r\n", Oline->id);
    }
    Oline = NULL;
    printf("victim line pq \r\n");
    for (int i = 1; i < ssd->lm.victim_line_pq->size ;i++) {
        struct line* victim_line = ssd->lm.victim_line_pq->d[i];
        printf("line->id %d, line->vpc %d, line->ipc %d \r\n", victim_line->id, victim_line->vpc, victim_line->ipc);
        // printf("line->id %d \r\n", victim_line->id);
    }
    printf("======= current line %d, line->vpc %d, line->ipc %d \r\n", ssd->wp.curline->id, ssd->wp.curline->vpc, ssd->wp.curline->ipc);

}

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
    ftl_assert(lpn < ssd->sp.logic_ttpgs);
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

    // ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static struct ppa pgidx2ppa(struct ssd *ssd, uint64_t idx)
{
    struct ppa ppa = {0};
    struct ssdparams *spp = &ssd->sp;

    if (idx >= spp->tt_pgs) {
        ppa.ppa = UNMAPPED_PPA;
        return ppa;
    }

    uint64_t original_idx = idx;

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
    ftl_assert(pgidx == original_idx);

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

    QTAILQ_INIT(&lm->backup_list);
    lm->se.sec_backup = false;
    // lm->se.secure_key = g_malloc0(sizeof(char) * 8);
    // lm->se.secure_key = "12345678";
    lm->se.secure_key = 12345678;
    lm->backup_cnt = 0;
    lm->backup_wp.curline = NULL;

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

/* modify to get young line for wear leveling*/

// static struct line *get_next_free_line(struct ssd *ssd, void *mb)
// {
//     struct line_mgmt *lm = &ssd->lm;
//     struct line *curline = NULL;

//     curline = QTAILQ_FIRST(&lm->free_line_list);
//     if (!curline) {
//         ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
//         return NULL;
//     }

//     QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
//     lm->free_line_cnt--;

//     // clean current line
//     struct ssdparams *spp = &ssd->sp;
//     int bytes_per_pages = spp->secsz * spp->secs_per_pg;
//     for(int ch=0; ch<spp->nchs; ch++){
//         for(int lun=0; lun<spp->luns_per_ch; lun++){
//             for(int pls=0; pls<spp->pls_per_lun; pls++){
//                 for(int pg=0; pg<spp->pgs_per_blk; pg++){
//                     struct ppa ppa;
//                     ppa.ppa = 0;
//                     ppa.g.ch = ch;
//                     ppa.g.lun = lun;
//                     ppa.g.pg = pg;
//                     ppa.g.blk = curline->id;
//                     ppa.g.pl = 0;
//                     uint64_t physical_page_num = ppa2pgidx(ssd, &ppa);
//                     memset(((char*)(mb + (physical_page_num * bytes_per_pages))), 0, bytes_per_pages);
//                     struct FG_OOB *old_OOB = &(ssd->OOB[physical_page_num]);
//                     old_OOB->LPA = INVALID_LPN;
//                     old_OOB->P_PPA = INVALID_PPA;
//                     old_OOB->Timestamp = INVALID_TIME;
//                     old_OOB->RIP = 0;
//                 }
//             }
//         }
//     }

//     return curline;
// }

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
                wpp->curline = get_young_block(ssd, mb);
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

static void ssd_init_file_mark(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->file_mark = g_malloc0(spp->logic_ttpgs);
    for (int i = 0; i < spp->logic_ttpgs; i++) {
        ssd->file_mark[i] = false;
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
        ssd->OOB[i].entropy = 0.0;
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
    ssd_init_file_mark(ssd);
    /* initialize OOB */
    ssd_init_OOB(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

    init_backup_wp(ssd, n->mbe->logical_space);

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
    struct nand_lun *new_lun;
    
    struct ppa latency_new_ppa;

    write_count++;

    if(type){
        uint64_t lpn = get_rmap_ent(ssd, old_ppa);
        ftl_assert(!valid_lpn(ssd, lpn));
        // if(lpn != old_OOB->LPA){
        if(lpn != INVALID_LPN){
            printf("RTT bug!\r\n");
            // printf("lpn : %lu, old_OOB->LPA : %lu\r\n", lpn, old_OOB->LPA);
            while(1);
        }
        latency_new_ppa = write_to_backup_block(ssd, old_ppa, mb);
        // current_OOB->LPA = old_OOB->LPA;
        // current_OOB->P_PPA = old_ppa->ppa;
        // current_OOB->Timestamp = old_OOB->Timestamp;
        // current_OOB->RIP = old_OOB->RIP;
        // if(type == SET_RIP) current_OOB->RIP = 1;
        // set_rmap_ent(ssd, INVALID_LPN, &new_ppa);
        // set_RTTbit(ssd, &new_ppa);
        // // mark_page_invalid(ssd, &new_ppa);
        // struct nand_block *blk = NULL;
        // struct nand_page *pg = NULL;
        // struct line *line;
        // pg = get_pg(ssd, &new_ppa);
        // pg->status = PG_INVALID;

        // /* update corresponding block status */
        // blk = get_blk(ssd, &new_ppa);
        // ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
        // blk->ipc++;

        // /* update corresponding line status */
        // line = get_line(ssd, &new_ppa);
        // ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
        // line->ipc++;
    }
    else{
        struct ppa new_ppa;
        uint64_t lpn = get_rmap_ent(ssd, old_ppa);
        ftl_assert(valid_lpn(ssd, lpn));
        if(lpn == INVALID_LPN){
            printf("normal bug!\r\n");
            // printf("lpn : %lu, old_OOB->LPA : %lu\r\n", lpn, old_OOB->LPA);
            while(1);
        }


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
        struct FG_OOB *old_OOB = &(ssd->OOB[old_physical_page_num]);
        struct FG_OOB *current_OOB = &(ssd->OOB[new_physical_page_num]);

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
        if(ssd->RTTtbl[old_physical_page_num]) set_RTTbit(ssd, &new_ppa);
        ssd_advance_write_pointer(ssd, mb);
        latency_new_ppa = new_ppa;
    }

    set_rmap_ent(ssd, INVALID_LPN, old_ppa);
    clr_RTTbit(ssd, old_ppa);
    /* need to advance the write pointer here */

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &latency_new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &latency_new_ppa);
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
    struct nand_block *blk_iter = NULL;
    int cnt = 0;
    int page_size = spp->secsz * spp->secs_per_pg;
    blk_iter = get_blk(ssd, ppa);
    // ftl_debug("GC-ing block:%d,ipc=%d,vpc=%d,erase_cnt=%d\n", ppa->g.blk,
    //           blk_iter->ipc, blk_iter->vpc, blk_iter->erase_cnt);
    // printf("++++++++++++++++++++++++++++++\r\n");
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
        else{
            printf("clean_one_block error\r\n");
            while(1);
        }
    }
    // printf("cnt: %d\r\n", cnt);
    // printf("blk_iter->vpc %d\r\n", blk_iter->vpc);
    // printf("++++++++++++++++++++++++++++++\r\n");
    ftl_assert(blk_iter->vpc == cnt);
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
    #ifdef GC_DEBUG
    printf("do_gc\r\n");
    #endif
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
    GC_count++;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }
    ppa.g.blk = victim_line->id;
    // ftl_debug("GC-ing line:%d,ipc=%d,vpc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
    //           victim_line->ipc, victim_line->vpc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
    //           ssd->lm.free_line_cnt);

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
    // if(req->is_file){
    //     for (lpn = start_lpn; lpn <= end_lpn; lpn++){
    //         printf("lpn: %lu\r\n", lpn);
    //     }
    // }
        
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
            char* for_hamming_distance = g_malloc0(bytes_per_pages);
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
                memcpy(for_hamming_distance, (char*)(mb + (old_physical_page_num * bytes_per_pages)), bytes_per_pages);
                
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
            
            write_count++;
            write_in_ssd_count++;
            // reduce backup
            if(req->is_file){
                ssd->file_mark[lpn] = true;
            }
            else{
                ssd->file_mark[lpn] = false;
            }

            if(ssd->file_mark[lpn])
            {
                if(mapped_ppa(&old_ppa)){
                    uint64_t old_physical_page_num = ppa2pgidx(ssd, &old_ppa);
                    if(ssd->RTTtbl[old_physical_page_num]){
                        #ifdef ENTROPY
                        float entropy_value_increase = calculate_entropy((unsigned char *)for_hamming_distance, (unsigned char *)rmw_R_buf, lba_off_in_page, cur_len);
                        if(entropy_value_increase < ENTROPY_INCREASE_THRESHOLD){
                            clr_RTTbit(ssd, &old_ppa);
                        }
                        #else
                        uint64_t hamming_value = calculate_hamming_distance(ssd, (uint64_t *)for_hamming_distance, (uint64_t *)rmw_R_buf, lba_off_in_page, cur_len);
                        if(hamming_value < cur_len/100){
                            clr_RTTbit(ssd, &old_ppa);
                        }
                        #endif
                    }
                }
            }
            
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
            g_free(for_hamming_distance);
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

struct recovery_meta {
    uint64_t LPA;
    uint32_t count;
    uint32_t version;
    uint16_t status;
};

struct time_list {
    uint64_t time;
    struct time_list *pre;
    struct time_list *next;
};

static void insert_time_list(struct time_list *head, uint64_t time){
    struct time_list *new_node = g_malloc0(sizeof(struct time_list));
    new_node->time = time;
    struct time_list *cur = head;
    while(cur->next != NULL){
        if(cur->next->time > time){
            break;
        }
        cur = cur->next;
    }
    new_node->next = cur->next;
    new_node->pre = cur;
    cur->next = new_node;
    if(new_node->next != NULL){
        new_node->next->pre = new_node;
    }
}

static void free_time_list(struct time_list *head){
    struct time_list *cur = head;
    while(cur->next != NULL){
        cur = cur->next;
        free(cur->pre);
    }
    free(cur);
}

static uint64_t timezone_recovery(struct ssd *ssd, FemuCtrl *n, uint64_t specify_time){
    printf("timezone_recovery\r\n");
    struct ssdparams *spp = &ssd->sp;
    void *mb = n->mbe->logical_space;
    struct ppa *build_maptbl;
    build_maptbl = g_malloc0(sizeof(struct ppa) * spp->logic_ttpgs);
    for (int i = 0; i < spp->logic_ttpgs; i++) {
        build_maptbl[i].ppa = UNMAPPED_PPA;
    }
    for (uint64_t phy = 0; phy < spp->tt_pgs; phy++) {
        if(ssd->OOB[phy].Timestamp > specify_time) continue;
        uint64_t phy_lpa = ssd->OOB[phy].LPA;
        if (phy_lpa == INVALID_LPN || phy_lpa >= spp->logic_ttpgs) continue;
        struct ppa curr_page;
        curr_page.ppa = build_maptbl[phy_lpa].ppa;
        struct line *line = NULL;
        bool in_free_line = false;
        struct ppa iter_ppa = pgidx2ppa(ssd, phy);
        QTAILQ_FOREACH(line, &ssd->lm.free_line_list, entry){
            if(line->id == iter_ppa.g.blk){
                in_free_line = true;
                break;
            }
        }
        if(in_free_line) continue;
        // printf("phy %lu \r\n", phy);
        if(curr_page.ppa == UNMAPPED_PPA){
            struct ppa got_ppa = pgidx2ppa(ssd, phy);
            build_maptbl[phy_lpa].ppa = got_ppa.ppa;
        }
        else{
            uint64_t curr_page_num = ppa2pgidx(ssd, &curr_page);
            if(ssd->OOB[phy].Timestamp > ssd->OOB[curr_page_num].Timestamp){
                struct ppa got_ppa = pgidx2ppa(ssd, phy);
                build_maptbl[phy_lpa].ppa = got_ppa.ppa;
            }
        }
    }

    printf("start update to new\r\n");
    for (uint64_t logic = 0; logic < spp->logic_ttpgs; logic++) {
        // update_to_new(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check);
        if(build_maptbl[logic].ppa != UNMAPPED_PPA) {
            if (build_maptbl[logic].ppa != ssd->maptbl[logic].ppa){
                update_to_new(ssd, &build_maptbl[logic], logic, mb);
            }
        }
    }
    printf("done update to new\r\n");

    g_free(build_maptbl);
    printf("recovery done\r\n");
    return 0;
}

#define RECOVERY_SUCCESS    0x0
#define RECOVERY_FAIL       0x1

uint16_t RA_recovery(FemuCtrl *n, NvmeCmd *cmd){    
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint32_t cdw10 = le32_to_cpu(cmd->cdw10);
    uint32_t cdw11 = le32_to_cpu(cmd->cdw11);
    uint32_t version = le32_to_cpu(cmd->cdw12);
    uint32_t complete = le32_to_cpu(cmd->cdw13);
    uint64_t LPA = cdw11;
    LPA = LPA << 32 | cdw10;
    
    struct recovery_meta rm;
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    printf("RA_recovery\r\n");
    rm.LPA = LPA;
    rm.count = 0;
    rm.version = 0;
    rm.status = RECOVERY_SUCCESS;
    struct time_list *head = g_malloc0(sizeof(struct time_list));
    head->time = 0;
    head->pre = NULL;
    struct time_list *cur = head;
    for(int i=0; i<spp->tt_pgs; i++){
        if(ssd->OOB[i].LPA == cdw10){
            rm.count++;
            insert_time_list(head, ssd->OOB[i].Timestamp);
        }
    }
    
    for(int i=0; i<rm.count; i++){
        printf("time: %lu\r\n", cur->next->time);
        cur = cur->next;
    }

    if(version || complete){
        if(version > rm.count || version == 0){
            rm.status = RECOVERY_FAIL;
            free_time_list(head);
            return dma_read_prp(n, (uint8_t *)&rm, sizeof(rm), prp1, prp2);
        }
        else{
            //do recovery
            cur = head;
            printf("specify_time: %lu\r\n", cur->time);
            for(int i=0; i<version; i++){
                cur = cur->next;
            }
            printf("specify_time: %lu\r\n", cur->time);
            timezone_recovery(ssd, n, cur->time);
            if(complete){
                for(int i=0; i<spp->tt_pgs; i++){
                    ssd->RTTtbl[i] = 0;
                }
            }
            rm.version = version;
        }
    }
    free_time_list(head);
    return dma_read_prp(n, (uint8_t *)&rm, sizeof(rm), prp1, prp2);
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
    req->is_file = (rw->rsvd2 & file_flag) >> 63;
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

static struct line *get_older_block(struct ssd *ssd, void *mb){
    uint64_t max_count = 0;
    struct line *target_line = NULL;
    target_line = QTAILQ_FIRST(&ssd->lm.free_line_list);
    struct line *line = NULL;
    
    QTAILQ_FOREACH(line, &ssd->lm.free_line_list, entry){
        struct ppa ppa;
        ppa.ppa = 0;
        ppa.g.blk = line->id;
        struct nand_block *blk = get_blk(ssd, &ppa);
        if(blk->erase_cnt > max_count){
            max_count = blk->erase_cnt;
            target_line = line;
        }
    }
    QTAILQ_REMOVE(&ssd->lm.free_line_list, target_line, entry);
    ssd->lm.free_line_cnt--;

    struct ssdparams *spp = &ssd->sp;
    int bytes_per_pages = spp->secsz * spp->secs_per_pg;
    for(int ch=0; ch<spp->nchs; ch++){
        for(int lun=0; lun<spp->luns_per_ch; lun++){
            for(int pls=0; pls<spp->pls_per_lun; pls++){
                for(int pg=0; pg<spp->pgs_per_blk; pg++){
                    struct ppa ppa;
                    ppa.ppa = 0;
                    ppa.g.ch = ch;
                    ppa.g.lun = lun;
                    ppa.g.pg = pg;
                    ppa.g.blk = target_line->id;
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
    }

    return target_line;
}

static struct line *get_young_block(struct ssd *ssd, void *mb){
    uint64_t min_count = 0xFFFFFFFFFFFFFFFF;
    struct line *target_line = NULL;
    target_line = QTAILQ_FIRST(&ssd->lm.free_line_list);
    struct line *line = NULL;
    
    QTAILQ_FOREACH(line, &ssd->lm.free_line_list, entry){
        struct ppa ppa;
        ppa.ppa = 0;
        ppa.g.blk = line->id;
        struct nand_block *blk = get_blk(ssd, &ppa);
        if(blk->erase_cnt < min_count){
            min_count = blk->erase_cnt;
            target_line = line;
        }
    }
    if(target_line == NULL){
        print_line_status(ssd);
        while(1);
    }
    QTAILQ_REMOVE(&ssd->lm.free_line_list, target_line, entry);
    ssd->lm.free_line_cnt--;

    struct ssdparams *spp = &ssd->sp;
    int bytes_per_pages = spp->secsz * spp->secs_per_pg;
    for(int ch=0; ch<spp->nchs; ch++){
        for(int lun=0; lun<spp->luns_per_ch; lun++){
            for(int pls=0; pls<spp->pls_per_lun; pls++){
                for(int pg=0; pg<spp->pgs_per_blk; pg++){
                    struct ppa ppa;
                    ppa.ppa = 0;
                    ppa.g.ch = ch;
                    ppa.g.lun = lun;
                    ppa.g.pg = pg;
                    ppa.g.blk = target_line->id;
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
    }

    return target_line;
}

static struct ppa get_new_backup_page(struct ssd *ssd){
    struct line_mgmt *lm = &ssd->lm;
    struct write_pointer *wpp = &lm->backup_wp;
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

static void init_backup_wp(struct ssd *ssd, void *mb){
    struct line_mgmt *lm = &ssd->lm;
    struct write_pointer *wpp = &lm->backup_wp;
    wpp->curline = get_older_block(ssd, mb);
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = wpp->curline->id;
    wpp->pl = 0;
}

static void backup_update_write_pointer(struct ssd *ssd, void *mb){
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct write_pointer *wpp = &lm->backup_wp;

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
                QTAILQ_INSERT_TAIL(&lm->backup_list, wpp->curline, entry);
                // struct line *tmp = NULL;
                // printf("backup list: ");
                // QTAILQ_FOREACH(tmp, &lm->backup_list, entry){
                //     printf("%d ", tmp->id);
                // }
                // printf("\r\n");
                lm->backup_cnt++;
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_older_block(ssd, mb);
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

static struct ppa write_to_backup_block(struct ssd *ssd, struct ppa *ppa, void *mb){
    struct ssdparams *spp = &ssd->sp;
    int bytes_per_pages = spp->secsz * spp->secs_per_pg;
    uint64_t physical_page_num = ppa2pgidx(ssd, ppa);
    struct FG_OOB *old_OOB = &(ssd->OOB[physical_page_num]);

    struct ppa new_ppa = get_new_backup_page(ssd);
    uint64_t new_physical_page_num = ppa2pgidx(ssd, &new_ppa);
    struct FG_OOB *new_OOB = &(ssd->OOB[new_physical_page_num]);

    memcpy((char*)(mb + (new_physical_page_num * bytes_per_pages)), (char*)(mb + (physical_page_num * bytes_per_pages)), bytes_per_pages);

    write_count++;
    backup_pages++;
    backup_update_write_pointer(ssd, mb);
    new_OOB->LPA = old_OOB->LPA;
    new_OOB->P_PPA = old_OOB->P_PPA;
    new_OOB->Timestamp = old_OOB->Timestamp;

    if(ssd->RTTtbl[physical_page_num]) {
        new_OOB->RIP = 1;
        set_RTTbit(ssd, &new_ppa);
    }
    else new_OOB->RIP = old_OOB->RIP;

    struct nand_page *pg = NULL;
    pg = get_pg(ssd, &new_ppa);
    pg->status = PG_INVALID;

    struct nand_block *blk = NULL;
    /* update corresponding block status */
    blk = get_blk(ssd, &new_ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;

    struct line *line = NULL;
    /* update corresponding line status */
    line = get_line(ssd, &new_ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    line->ipc++;
    return new_ppa;
}

static void clean_line(struct ssd *ssd, void *mb, struct line* line, bool sanitize){
    struct ssdparams *spp = &ssd->sp;
    uint64_t bytes_per_pages = spp->secsz * spp->secs_per_pg;
    for(int ch=0; ch<spp->nchs; ch++){
        for(int lun=0; lun<spp->luns_per_ch; lun++){
            for(int pls=0; pls<spp->pls_per_lun; pls++){
                struct nand_block *blk_iter = NULL;
                for(int pg=0; pg<spp->pgs_per_blk; pg++){
                    struct nand_page *pg_iter = NULL;
                    struct ppa ppa;
                    ppa.ppa = 0;
                    ppa.g.ch = ch;
                    ppa.g.lun = lun;
                    ppa.g.blk = line->id;
                    ppa.g.pl = pls;
                    ppa.g.pg = pg;
                    pg_iter = NULL;
                    pg_iter = get_pg(ssd, &ppa);
                    if (pg_iter == NULL) {
                        printf("Error: pg_iter is NULL\n");
                        while(1);
                    }
                    uint64_t physical_page_num = ppa2pgidx(ssd, &ppa);
                    bool is_valid = (pg_iter->status == PG_VALID);
                    bool is_invalid = (pg_iter->status == PG_INVALID);
                    bool is_invalid_and_RTT = (is_invalid && ssd->RTTtbl[physical_page_num]);
                    if(!sanitize){
                        if(is_valid || is_invalid_and_RTT){
                            // continue;
                            write_to_backup_block(ssd, &ppa, mb);
                        }
                    }
                    memset((char*)(mb + (physical_page_num * bytes_per_pages)), 0, bytes_per_pages);
                    struct FG_OOB *old_OOB = &(ssd->OOB[physical_page_num]);
                    old_OOB->LPA = INVALID_LPN;
                    old_OOB->P_PPA = INVALID_PPA;
                    old_OOB->Timestamp = INVALID_TIME;
                    old_OOB->RIP = 0;
                    clr_RTTbit(ssd, &ppa);
                    blk_iter = NULL;
                    blk_iter = get_blk(ssd, &ppa);
                    if(is_valid){
                        blk_iter->vpc--;
                        line->vpc--;
                    }
                    else if(is_invalid){
                        blk_iter->ipc--;
                        line->ipc--;
                    }
                    pg_iter->status = PG_FREE;
                }
                assert(blk_iter->ipc == 0 && blk_iter->vpc == 0);                
            }
        }
    }
    assert(line->ipc == 0 && line->vpc == 0);
    line->ipc = 0;
    line->vpc = 0;
    line->pos = 0;
    /* initialize all the lines as free lines */
    QTAILQ_INSERT_TAIL(&ssd->lm.free_line_list, line, entry);
    ssd->lm.free_line_cnt++;
}

// recovery fail after secure erase
static uint64_t ssd_secure_erase(struct ssd *ssd, FemuCtrl *n){
    printf("secure_erase \r\n");
    struct ssdparams *spp = &ssd->sp;
    void *mb = n->mbe->logical_space;

    bool sanitize = false;
    if(ssd->lm.se.secure_key == n->sec_argument){
        sanitize = true;
        ssd->lm.se.sec_backup = false;
    }
    else if(ssd->lm.se.sec_backup == false){
        sanitize = false;
        // ssd->lm.se.sec_backup = true;
        // init_backup_wp(ssd, mb);
    }
// modify to line clean
    struct line *line = NULL;
    struct line *temp = NULL;
    QTAILQ_INSERT_TAIL(&ssd->lm.full_line_list, ssd->wp.curline, entry);
    ssd->lm.full_line_cnt++;
    ssd->wp.curline = NULL;
    // QTAILQ_FOREACH(line, &ssd->lm.full_line_list, entry){
    //     printf("id: %d\r\n", line->id);
    //     clean_line(ssd, mb, line, sanitize);
    //     ssd->lm.full_line_cnt--;
    // }
    QTAILQ_FOREACH_SAFE(line, &ssd->lm.full_line_list, entry, temp) {
        clean_line(ssd, mb, line, sanitize);
        ssd->lm.full_line_cnt--;
    }
    for (int i = 1; i < ssd->lm.victim_line_pq->size ;i++) {
        struct line* victim_line = ssd->lm.victim_line_pq->d[i];
        clean_line(ssd, mb, victim_line, sanitize);
        ssd->lm.victim_line_cnt--;
    }



    // clean current line
    // int bytes_per_pages = spp->secsz * spp->secs_per_pg;
    // struct nand_page *pg_iter = NULL;
    // struct nand_block *blk_iter = NULL;
    // struct line *line = NULL;
    // printf("secure_erase start \r\n");
    // for(int ch=0; ch<spp->nchs; ch++){
    //     for(int lun=0; lun<spp->luns_per_ch; lun++){
    //         for(int pls=0; pls<spp->pls_per_lun; pls++){
    //             for(int blk=0; blk<spp->blks_per_pl; blk++){
    //                 line = NULL;
    //                 bool in_older_blk_using = false;
    //                 QTAILQ_FOREACH(line, &ssd->lm.backup_list, entry){
    //                     if(line->id == blk){
    //                         in_older_blk_using = true;
    //                         break;
    //                     }
    //                 }
    //                 if(in_older_blk_using || (ssd->lm.backup_wp.blk == blk)){
    //                     continue;
    //                 }
    //                 for(int pg=0; pg<spp->pgs_per_blk; pg++){
    //                     struct ppa ppa;
    //                     ppa.ppa = 0;
    //                     ppa.g.ch = ch;
    //                     ppa.g.lun = lun;
    //                     ppa.g.blk = blk;
    //                     ppa.g.pl = pls;
    //                     ppa.g.pg = pg;
    //                     pg_iter = NULL;
    //                     pg_iter = get_pg(ssd, &ppa);
    //                     if (pg_iter == NULL) {
    //                         printf("Error: pg_iter is NULL\n");
    //                         while(1);
    //                     }
    //                     uint64_t physical_page_num = ppa2pgidx(ssd, &ppa);
    //                     bool is_valid = (pg_iter->status == PG_VALID);
    //                     bool is_invalid = (pg_iter->status == PG_INVALID);
    //                     bool is_invalid_and_RTT = (is_invalid && ssd->RTTtbl[physical_page_num]);
    //                     if(!sanitize){
    //                         if(is_valid || is_invalid_and_RTT){
    //                             // continue;
    //                             write_to_backup_block(ssd, &ppa, mb);
    //                         }
    //                     }
    //                     memset((char*)(mb + (physical_page_num * bytes_per_pages)), 0, bytes_per_pages);
    //                     struct FG_OOB *old_OOB = &(ssd->OOB[physical_page_num]);
    //                     old_OOB->LPA = INVALID_LPN;
    //                     old_OOB->P_PPA = INVALID_PPA;
    //                     old_OOB->Timestamp = INVALID_TIME;
    //                     old_OOB->RIP = 0;
    //                     clr_RTTbit(ssd, &ppa);
    //                     // if(blk == 1 && pg==0){
    //                     //     printf("blk_iter->ipc %d \r\n", blk_iter->ipc);
    //                     //     printf("blk_iter->vpc %d \r\n", blk_iter->vpc);
    //                     // }
    //                     blk_iter = NULL;
    //                     blk_iter = get_blk(ssd, &ppa);
    //                     if (blk_iter == NULL) {
    //                         printf("Error: blk_iter is NULL\n");
    //                         while(1);
    //                     }
    //                     line = NULL;
    //                     line = get_line(ssd, &ppa);
    //                     if (line == NULL) {
    //                         printf("Error: line is NULL\n");
    //                         while(1);
    //                     }
    //                     if(is_valid){
    //                         blk_iter->vpc--;
    //                         line->vpc--;
    //                     }
    //                     else if(is_invalid){
    //                         blk_iter->ipc--;
    //                         line->ipc--;
    //                     }
    //                     pg_iter->status = PG_FREE;
    //                 }
    //                 assert(blk_iter->ipc == 0 && blk_iter->vpc == 0);
    //             }
    //         }
    //     }
    // }
    // printf("secure_erase end \r\n");
    for (int i = 0; i < spp->logic_ttpgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
        ssd->file_mark[i] = false;
    }
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
    struct line_mgmt *lm = &ssd->lm;
    // QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);
    if(sanitize){
        QTAILQ_INIT(&lm->backup_list);
        lm->backup_cnt = 0;
    }

    // struct line *clear_line;
    // lm->free_line_cnt = 0;
    // for (int i = 0; i < lm->tt_lines; i++) {
    //     line = NULL;
    //     bool in_older_blk_using = false;
    //     QTAILQ_FOREACH(line, &ssd->lm.backup_list, entry){
    //         if(line->id == i){
    //             in_older_blk_using = true;
    //             break;
    //         }
    //     }
    //     if(in_older_blk_using || ssd->lm.backup_wp.blk == i){
    //         continue;
    //     }
    //     clear_line = &lm->lines[i];
    //     clear_line->id = i;
    //     clear_line->ipc = 0;
    //     clear_line->vpc = 0;
    //     clear_line->pos = 0;
    //     /* initialize all the lines as free lines */
    //     QTAILQ_INSERT_TAIL(&lm->free_line_list, clear_line, entry);
    //     lm->free_line_cnt++;
    // }

    /* wpp->curline is always our next-to-write super-block */
    struct write_pointer *wpp = &ssd->wp;
    ftl_assert(wpp->curline == NULL);
    wpp->curline = get_young_block(ssd, mb);
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = wpp->curline->id;
    wpp->pl = 0;
    uint64_t all_lines = lm->free_line_cnt + lm->backup_cnt;
    if(lm->backup_wp.curline) all_lines++;
    if(ssd->wp.curline) all_lines++;
    ftl_assert(all_lines == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;

    n->sec_erase = 127;
    return 0;
    
}

static struct ppa copy_to_new_page(struct ssd *ssd, struct ppa *old_ppa, void *mb){ 
    struct ppa new_ppa;
    struct ssdparams *spp = &ssd->sp;
    int bytes_per_pages = spp->secsz * spp->secs_per_pg;
    uint64_t old_physical_page_num = ppa2pgidx(ssd, old_ppa);
    uint64_t old_start_addr = (uint64_t)mb + (old_physical_page_num * bytes_per_pages);
    uint64_t lpn = ssd->OOB[old_physical_page_num].LPA;

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd);
    // printf("new_ppa.ppa %lu \r\n", new_ppa.ppa);

    uint64_t new_physical_page_num = ppa2pgidx(ssd, &new_ppa);
    uint64_t new_start_addr = (uint64_t)mb + (new_physical_page_num * bytes_per_pages);
    
    memcpy((char*)new_start_addr, (char*)old_start_addr, bytes_per_pages);

    // check data correct
    for(int i=0; i<bytes_per_pages; i++){
        if(((char*)new_start_addr)[i] != ((char*)old_start_addr)[i]){
            printf("data move error, data incorrect\r\n");
        }
    }

    write_count++;

    struct FG_OOB *old_OOB = &(ssd->OOB[old_physical_page_num]);
    struct FG_OOB *current_OOB = &(ssd->OOB[new_physical_page_num]);
    current_OOB->LPA = old_OOB->LPA;
    current_OOB->P_PPA = INVALID_PPA;
    current_OOB->Timestamp = old_OOB->Timestamp;
    current_OOB->RIP = 0;
    return new_ppa;
}
static void update_to_new(struct ssd *ssd, struct ppa *build_ppa, uint64_t logic, void *mb){ 
    // move data to new page

    uint64_t phy = ppa2pgidx(ssd, build_ppa);
    struct ssdparams *spp = &ssd->sp;

    struct nand_page *pg = NULL;
    struct nand_block *blk = NULL;
    struct line *line = NULL;
    struct line_mgmt *lm = &ssd->lm;
    if(lm->se.sec_backup){
        struct ppa move_ppa = copy_to_new_page(ssd, build_ppa, mb);
        set_rmap_ent(ssd, ssd->OOB[phy].LPA, &move_ppa);
        set_maptbl_ent(ssd, ssd->OOB[phy].LPA, &move_ppa);
        mark_page_valid(ssd, &move_ppa);
        ssd_advance_write_pointer(ssd, mb);
    }
    else{
        struct ppa now_ppa = get_maptbl_ent(ssd, logic);
        uint64_t now_ppa_num = ppa2pgidx(ssd, &now_ppa);
        if (now_ppa_num == phy || now_ppa.ppa == INVALID_PPA) {
            return;
        }
        if (ssd->OOB[phy].LPA == ssd->OOB[now_ppa_num].LPA) {
            // printf("build_ppa->g.blk %d \r\n", build_ppa->g.blk);
            set_rmap_ent(ssd, INVALID_LPN, &now_ppa);
            set_rmap_ent(ssd, ssd->OOB[phy].LPA, build_ppa);
            set_maptbl_ent(ssd, ssd->OOB[phy].LPA, build_ppa);

            /* update page status */
            pg = NULL;
            pg = get_pg(ssd, build_ppa);
            pg->status = PG_VALID;

            /* update corresponding block status */
            blk = NULL;
            blk = get_blk(ssd, build_ppa);
            ftl_assert(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
            blk->vpc++;
            ftl_assert(blk->ipc > 0 && blk->ipc <= spp->pgs_per_blk);
            blk->ipc--;

            /* update corresponding line status */
            line = NULL;
            line = get_line(ssd, build_ppa);
            ftl_assert(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
            line->vpc++;
            ftl_assert(line->ipc > 0 && line->ipc <= spp->pgs_per_line);
            line->ipc--;

            // if this line become full, i need to move this line from victim list to full list
            if (line->vpc == spp->pgs_per_line) {
                /* move line: "victim" -> "full" */
                pqueue_remove(lm->victim_line_pq, line);
                lm->victim_line_cnt--;
                QTAILQ_INSERT_TAIL(&lm->full_line_list, line, entry);
                lm->full_line_cnt++;
            }

            bool was_full_line = false;
            pg = NULL;
            pg = get_pg(ssd, &now_ppa);
            pg->status = PG_INVALID;

            blk = NULL;
            blk = get_blk(ssd, &now_ppa);
            ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
            blk->ipc++;
            ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
            blk->vpc--;

            /* update corresponding line status */
            line = NULL;
            line = get_line(ssd, &now_ppa);
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
                QTAILQ_REMOVE(&lm->full_line_list, line, entry);
                lm->full_line_cnt--;
                pqueue_insert(lm->victim_line_pq, line);
                lm->victim_line_cnt++;
            }
        }
        else{
            printf("update_to_new fail \r\n");
            while(1);
        }
    }
    
}

static uint64_t do_recovery_new_version(struct ssd *ssd, FemuCtrl *n){
    printf("time zone recovery \r\n");
    struct ssdparams *spp = &ssd->sp;
    void *mb = n->mbe->logical_space;
    struct ppa *build_maptbl;
    build_maptbl = g_malloc0(sizeof(struct ppa) * spp->logic_ttpgs);
    for (int i = 0; i < spp->logic_ttpgs; i++) {
        build_maptbl[i].ppa = UNMAPPED_PPA;
    }
    for (uint64_t phy = 0; phy < spp->tt_pgs; phy++) {
        uint64_t phy_lpa = ssd->OOB[phy].LPA;
        if (phy_lpa == INVALID_LPN || phy_lpa >= spp->logic_ttpgs) continue;
        struct ppa curr_page;
        curr_page.ppa = build_maptbl[phy_lpa].ppa;
        struct line *line = NULL;
        bool in_free_line = false;
        struct ppa iter_ppa = pgidx2ppa(ssd, phy);
        QTAILQ_FOREACH(line, &ssd->lm.free_line_list, entry){
            if(line->id == iter_ppa.g.blk){
                in_free_line = true;
                break;
            }
        }
        if(in_free_line) continue;
        // printf("phy %lu \r\n", phy);
        if(curr_page.ppa == UNMAPPED_PPA){
            struct ppa got_ppa = pgidx2ppa(ssd, phy);
            build_maptbl[phy_lpa].ppa = got_ppa.ppa;
        }
        else{
            uint64_t curr_page_num = ppa2pgidx(ssd, &curr_page);
            if(ssd->OOB[phy].Timestamp > ssd->OOB[curr_page_num].Timestamp){
                struct ppa got_ppa = pgidx2ppa(ssd, phy);
                build_maptbl[phy_lpa].ppa = got_ppa.ppa;
            }
        }
    }

    uint64_t target_lpa = n->sec_argument;
    // struct ppa cur_target_ppa = get_maptbl_ent(ssd, target_lpa);
    struct ppa cur_target_ppa;
    cur_target_ppa.ppa = build_maptbl[target_lpa].ppa;
    uint64_t cur_target_ppa_num = ppa2pgidx(ssd, &cur_target_ppa);
    int64_t cur_target_timestamp = ssd->OOB[cur_target_ppa_num].Timestamp; // assume this ransomware attack
    /*     this is recover    */
    char *check = g_malloc0(spp->tt_pgs);
    int64_t original_delta = cur_target_timestamp;
    uint64_t pre_ppa_num = 0;
    for (uint64_t phy = 0; phy < spp->tt_pgs; phy++) {
        if(ssd->OOB[phy].LPA == target_lpa){
            int64_t delta = abs(ssd->OOB[phy].Timestamp - cur_target_timestamp);
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
                // struct ppa now_ppa = get_maptbl_ent(ssd, lpn_iter_ppa);
                struct ppa now_ppa;
                now_ppa.ppa = build_maptbl[lpn_iter_ppa].ppa;
                uint64_t now_ppa_num = ppa2pgidx(ssd, &now_ppa);
                // if(pre_ppa_timestamp > ssd->OOB[phy].Timestamp){
                if(abs(pre_ppa_timestamp - ssd->OOB[phy].Timestamp) < abs(pre_ppa_timestamp - ssd->OOB[now_ppa_num].Timestamp)){
                    if (ssd->OOB[phy].LPA == ssd->OOB[now_ppa_num].LPA){
                        // printf("swap in l2p, lpa %lu\r\n", lpn_iter_ppa);
                        build_maptbl[lpn_iter_ppa].ppa = iter_ppa.ppa;
                        check[now_ppa_num] = 1;
                    }
                    // update_to_new(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check);
                }
            }
        }
        check[phy] = 1;
    }
    g_free(check);
    printf("start update to new\r\n");
    for (uint64_t logic = 0; logic < spp->logic_ttpgs; logic++) {
        // update_to_new(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check);
        if(build_maptbl[logic].ppa != UNMAPPED_PPA) {
            if (build_maptbl[logic].ppa != ssd->maptbl[logic].ppa){
                update_to_new(ssd, &build_maptbl[logic], logic, mb);
            }
        }
        // printf("logic %d,  %lu, %lu\r\n", logic, build_maptbl[logic].ppa, ssd->maptbl[logic].ppa);
    }
    printf("done update to new\r\n");

    if(ssd->lm.se.sec_backup){
        struct line *line = NULL;
        struct nand_page *pg_iter = NULL;
        struct nand_block *blk_iter = NULL;
        if(ssd->lm.backup_wp.curline) QTAILQ_INSERT_TAIL(&ssd->lm.backup_list, ssd->lm.backup_wp.curline, entry);
        QTAILQ_FOREACH(line, &ssd->lm.backup_list, entry){
            struct ppa ppa;
            for(int ch=0; ch<spp->nchs; ch++){
                for(int lun=0; lun<spp->luns_per_ch; lun++){
                    for(int pls=0; pls<spp->pls_per_lun; pls++){
                        int blk = line->id;
                        ppa.ppa = 0;
                        ppa.g.ch = ch;
                        ppa.g.lun = lun;
                        ppa.g.blk = blk;
                        ppa.g.pl = pls;
                        blk_iter = get_blk(ssd, &ppa);
                        blk_iter->ipc = 0;
                        blk_iter->vpc = 0;
                        for(int pg=0; pg<spp->pgs_per_blk; pg++){
                            for(int pg=0; pg<spp->pgs_per_blk; pg++){
                                ppa.g.pg = pg;
                                pg_iter = get_pg(ssd, &ppa);
                                pg_iter->status = PG_FREE;
                            }
                        }
                    }
                }
            }
            line->ipc = 0;
            line->vpc = 0;
            line->pos = 0;
            /* initialize all the lines as free lines */
            QTAILQ_INSERT_TAIL(&ssd->lm.free_line_list, line, entry);
            ssd->lm.free_line_cnt++;
        }
        QTAILQ_INIT(&ssd->lm.backup_list);
        ssd->lm.se.sec_backup = false;
        ssd->lm.backup_wp.curline = NULL;
        ssd->lm.backup_cnt = 0;
    }

    g_free(build_maptbl);
    printf("recovery done\r\n");
    n->sec_erase = 127;
    return 0;
}

/* first version - just back one version for each lpa */
// static uint64_t do_recovery(struct ssd *ssd, FemuCtrl *n){
//     printf("recovery \r\n");
//     /*     this is recover    */
//     struct ssdparams *spp = &ssd->sp;
//     char *check = g_malloc0(spp->tt_pgs);
//     for (uint64_t phy = 0; phy < spp->tt_pgs; phy++) {
//         // printf("phy %lu \r\n", phy);
//         struct nand_page *pg_iter = NULL;
//         if(!check[phy]){
//             struct ppa iter_ppa = pgidx2ppa(ssd, phy);
//             pg_iter = get_pg(ssd, &iter_ppa);
//             if(ssd->RTTtbl[phy] == 1 && pg_iter->status == PG_INVALID){
//                 uint64_t lpn_iter_ppa = ssd->OOB[phy].LPA;
//                 struct ppa now_ppa = get_maptbl_ent(ssd, lpn_iter_ppa);
//                 uint64_t now_ppa_num = ppa2pgidx(ssd, &now_ppa);
//                 if(ssd->OOB[phy].RIP){
//                     if(!check[now_ppa_num] && ssd->OOB[phy].Timestamp < ssd->OOB[now_ppa_num].Timestamp){
//                         swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check);
//                     }
//                     else if(ssd->OOB[phy].Timestamp > ssd->OOB[now_ppa_num].Timestamp){
//                         swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check);
//                     }
//                 }
//                 else{
//                     if(!check[now_ppa_num] && ssd->OOB[phy].Timestamp < ssd->OOB[now_ppa_num].Timestamp){
//                         swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check);
//                     }
//                     else if(ssd->OOB[phy].Timestamp > ssd->OOB[now_ppa_num].Timestamp){
//                         swap_in_l2p(ssd, phy, now_ppa_num, iter_ppa, now_ppa, check);
//                     }
//                 }
//             }
//         }
//         check[phy] = 1;
//     }
//     g_free(check);

//     printf("recovery done\r\n");
//     n->sec_erase = 127;
//     return 0;
// }

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
    return NULL;
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
    for (uint64_t logic = 0; logic < spp->logic_ttpgs; logic++) {
        struct ppa ppa = get_maptbl_ent(ssd, logic);
        if (ppa.ppa != INVALID_PPA) {
            fprintf(file, "logic %ld: ch %d, lun %d, plane %d, block %d, page %d, timestamp %lu\n", 
                    logic, ppa.g.ch, ppa.g.lun, ppa.g.pl, ppa.g.blk, ppa.g.pg, ssd->OOB[ppa2pgidx(ssd, &ppa)].Timestamp);
        }
        else{
            fprintf(file, "logic %ld: physical %d\n", logic, -1);
        }
    }
    fclose(file);
    n->sec_erase = 127;

    
    for (uint64_t phy = 0; phy < spp->tt_pgs; phy++) {
        if(ssd->OOB[phy].LPA == n->sec_argument){
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
    for (uint64_t logic = 0; logic < spp->logic_ttpgs; logic++) {
        struct ppa ppa = get_maptbl_ent(ssd, logic);
        if (ppa.ppa != INVALID_PPA) {
            fprintf(file, "logic %ld: ch %d, lun %d, plane %d, block %d, page %d, timestamp %lu\n", 
                    logic, ppa.g.ch, ppa.g.lun, ppa.g.pl, ppa.g.blk, ppa.g.pg, ssd->OOB[ppa2pgidx(ssd, &ppa)].Timestamp);
        }
        else{
            fprintf(file, "logic %ld: physical %d\n", logic, -1);
        }
    }
    fclose(file);
    
    // QemuThread threads[THREAD_NUM];
    // struct SsdMbPackage thread_ids[THREAD_NUM];

    // for (int i = 0; i < THREAD_NUM; i++) {
    //     thread_ids[i].id = i;
    //     thread_ids[i].mb = mb;
    //     thread_ids[i].ssd = ssd;
    //     thread_ids[i].n = n;
    //     qemu_thread_create(&threads[i], "worker", worker, &thread_ids[i], QEMU_THREAD_JOINABLE);
    // }

    // // 等待所有執行緒完成
    // for (int i = 0; i < THREAD_NUM; i++) {
    //     qemu_thread_join(&threads[i]);
    // }
    uint64_t num_backup = 0;
    uint64_t num_RTT = 0;
    uint64_t file_cnt = 0;
    uint64_t valid_page_cnt = 0;
    for(int i=0; i<spp->tt_pgs; i++){
        if(ssd->RTTtbl[i] == 1){
            struct nand_page *pg = NULL;
            struct ppa check_ppa;
            check_ppa = pgidx2ppa(ssd, i);
            pg = get_pg(ssd, &check_ppa);
            if (pg->status == PG_INVALID)
                num_backup++;
            // printf("lpas %lu,\r\n", ssd->OOB[i].LPA);
            num_RTT++;
        }
        if(i<spp->logic_ttpgs){
            if(ssd->file_mark[i]){
                file_cnt++;
            }
            if(ssd->maptbl[i].ppa != INVALID_PPA){
                valid_page_cnt++;
            }
        }
    }
    uint64_t blk_erase_max = 0;
    uint64_t blk_erase_min = 0xFFFFFFFFFFFFFFFF;
    uint64_t blk_erase_avg = 0;
    for(int i=0; i<spp->blks_per_lun; i++){
        struct ppa blk_ppa;
        blk_ppa.ppa = 0;
        blk_ppa.g.blk = i;
        struct nand_block *blk = get_blk(ssd, &blk_ppa);
        if(blk->erase_cnt < blk_erase_min){
            blk_erase_min = blk->erase_cnt;
        }
        if(blk->erase_cnt > blk_erase_max){
            blk_erase_max = blk->erase_cnt;
        }
        blk_erase_avg += blk->erase_cnt;
    }
    blk_erase_avg /= spp->blks_per_lun;
    double sumSquaredDifferences = 0.0;

    for(int i=0; i<spp->blks_per_lun; i++){
        struct ppa blk_ppa;
        blk_ppa.ppa = 0;
        blk_ppa.g.blk = i;
        struct nand_block *blk = get_blk(ssd, &blk_ppa);
        double mean = blk_erase_avg;
        sumSquaredDifferences += pow(blk->erase_cnt - mean, 2);
    }
    sumSquaredDifferences = sqrt(sumSquaredDifferences / spp->blks_per_lun);
    
    printf("backup_pages %lu\r\n", backup_pages);
    printf("num_backup %lu\r\n", num_backup);
    printf("num_RTT %lu\r\n", num_RTT);
    printf("GC_count %lu\r\n", GC_count);
    printf("file_cnt %lu\r\n", file_cnt);
    printf("write_count %lu\r\n", write_count);
    printf("write_in_ssd_count %lu\r\n", write_in_ssd_count);
    printf("valid_page_cnt %lu\r\n", valid_page_cnt);
    printf("blk_erase_max %lu\r\n", blk_erase_max);
    printf("blk_erase_min %lu\r\n", blk_erase_min);
    printf("blk_erase_avg %lu\r\n", blk_erase_avg);
    printf("sumSquaredDifferences %f\r\n", sumSquaredDifferences);

    
    printf("\r\nwrite file successful\r\n");
    
    n->sec_erase = 127;
    return 0;
}

static float calculate_entropy(unsigned char *for_hamming_distance, unsigned char *rmw_R_buf, uint64_t lba_off_in_page, uint64_t cur_len) {
    int ori_frequency[256] = {0};
    int new_frequency[256] = {0};
    size_t i;

    for (i = lba_off_in_page; i < cur_len; i++) {
        ori_frequency[for_hamming_distance[i]]++;
        new_frequency[rmw_R_buf[i]]++;
    }

    float ori_entropy = 0.0;
    float new_entropy = 0.0;
    for (i = 0; i < 256; i++) {
        if (ori_frequency[i] > 0) {
            float p = (float)ori_frequency[i] / cur_len;
            ori_entropy -= p * log2(p);
        }
        if (new_frequency[i] > 0) {
            float p = (float)new_frequency[i] / cur_len;
            new_entropy -= p * log2(p);
        }
    }
    float entropy = new_entropy / ori_entropy;
    return entropy;
}

uint64_t calculate_hamming_distance(struct ssd *ssd, uint64_t *data1, uint64_t *data2, uint64_t offset, uint64_t length) {
    uint64_t hamming_distance = 0;
    for (int i = offset; i < length/8; i++) {
        uint64_t xor_result = data1[i] ^ data2[i];
        hamming_distance += __builtin_popcountll(xor_result);
    }
    return hamming_distance/8;
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
        if(ssd->lm.backup_cnt) ssd->lm.se.sec_backup = true;
        if(n->sec_erase == 0) dump_p2l(ssd, n);
        if(n->sec_erase == 1) ssd_secure_erase(ssd, n);
        else if(n->sec_erase == 2) {
            // printf("n->sec_argument %llu\r\n", n->sec_argument);
            // do_recovery(ssd, n);
            do_recovery_new_version(ssd, n);
        }
        else if(n->sec_erase == 3) dump(ssd, n);
        // if(!n->file_solved){
        //     n->file_solved = 1;
        //     uint64_t lpa = n->file_offset / ssd->sp.secs_per_pg;
        //     if(n->file_size > 4096){
        //         int iter = n->file_size % 4096 ? n->file_size / 4096 + 1 : n->file_size / 4096;
        //         for(int i=lpa; i<lpa+iter; i++){
        //             ssd->file_mark[i] = 1;
        //         }
        //     }
        //     else{
        //         ssd->file_mark[lpa] = 1;
        //     }
        //     // printf("===========\r\n");
        //     // printf("file_lpa %llu\r\n", lpa);
        //     // printf("file_length %llu\r\n", n->file_size);
        //     // printf("file solved\r\n");
        // }
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
