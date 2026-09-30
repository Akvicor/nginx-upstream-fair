/*
 * Copyright (C) 2007 Grzegorz Nosek
 * Work sponsored by Ezra Zygmuntowicz & EngineYard.com
 *
 * Based on nginx source (C) Igor Sysoev
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "peers/ngx_http_upstream_fair_peers.h"
#if (NGX_HTTP_UPSTREAM_CHECK)
#include "ngx_http_upstream_check_module.h"
#endif

/* 扩展标志随 upstream flags 传给主备两组。 */
#define NGX_HTTP_UPSTREAM_FAIR_NO_RR            (1<<26)
#define NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_IDLE (1<<27)
#define NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_PEAK (1<<28)
#define NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_MASK ((1<<27) | (1<<28))

enum { WM_DEFAULT = 0, WM_IDLE, WM_PEAK };

#define NGX_PEER_INVALID ((ngx_uint_t) -1)

/* 位图随请求和实际选中的组切换；告警保存快照，解锁后输出。 */
typedef struct {
    ngx_http_upstream_fair_peers_t *peers;
    ngx_uint_t current;
    uintptr_t *tried;
    uintptr_t *done;
    uintptr_t data;
    uintptr_t data2;
    size_t bitmap_size;
    ngx_uint_t negative_peer;
    ngx_int_t negative_nreq;
    unsigned single:1;
} ngx_http_upstream_fair_peer_data_t;

static ngx_int_t ngx_http_upstream_init_fair(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_get_fair_peer(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_free_fair_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t state);
static ngx_int_t ngx_http_upstream_init_fair_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
static char *ngx_http_upstream_fair(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_upstream_fair_set_shm_size(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

#if (NGX_HTTP_SSL)
static ngx_int_t ngx_http_upstream_fair_set_session(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_fair_save_session(ngx_peer_connection_t *pc,
    void *data);
#endif

static ngx_command_t ngx_http_upstream_fair_commands[] = {
    { ngx_string("fair"),
      NGX_HTTP_UPS_CONF|NGX_CONF_ANY,
      ngx_http_upstream_fair, 0, 0, NULL },
    { ngx_string("upstream_fair_shm_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_fair_set_shm_size, NGX_HTTP_MAIN_CONF_OFFSET, 0, NULL },
    ngx_null_command
};

static ngx_http_module_t ngx_http_upstream_fair_module_ctx = {
    NULL, NULL,
    ngx_http_upstream_fair_create_main_conf, NULL,
    NULL, NULL,
    NULL, NULL
};

ngx_module_t ngx_http_upstream_fair_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_fair_module_ctx,
    ngx_http_upstream_fair_commands,
    NGX_HTTP_MODULE,
    NULL,
    NULL,
    ngx_http_upstream_fair_init_process,
    NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};

#define NGX_BITVECTOR_ELT_SIZE (sizeof(uintptr_t) * 8)

static uintptr_t *
ngx_bitvector_alloc(ngx_pool_t *pool, ngx_uint_t size, uintptr_t *small)
{
    ngx_uint_t nelts = (size + NGX_BITVECTOR_ELT_SIZE - 1) / NGX_BITVECTOR_ELT_SIZE;

    if (small && nelts == 1) {
        *small = 0;
        return small;
    }
    return ngx_pcalloc(pool, nelts * sizeof(uintptr_t));
}

static ngx_int_t
ngx_bitvector_test(uintptr_t *bv, ngx_uint_t bit)
{
    return bv[bit / NGX_BITVECTOR_ELT_SIZE]
           & ((uintptr_t) 1 << (bit % NGX_BITVECTOR_ELT_SIZE));
}

static void
ngx_bitvector_set(uintptr_t *bv, ngx_uint_t bit)
{
    bv[bit / NGX_BITVECTOR_ELT_SIZE] |= (uintptr_t) 1 << (bit % NGX_BITVECTOR_ELT_SIZE);
}

static char *
ngx_http_upstream_fair_set_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_fair_main_conf_t *main = conf;
    ssize_t size;
    ngx_str_t *value = cf->args->elts;

    size = ngx_parse_size(&value[1]);
    if (size == NGX_ERROR || (size_t) size > SIZE_MAX - (ngx_pagesize - 1)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "Invalid memory area size `%V'", &value[1]);
        return NGX_CONF_ERROR;
    }
    main->shm_size = ngx_align((size_t) size, ngx_pagesize);
    if (main->shm_size < 8 * ngx_pagesize) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "The upstream_fair_shm_size value must be at least %uzKiB",
                           (8 * ngx_pagesize) >> 10);
        main->shm_size = 8 * ngx_pagesize;
    }
    ngx_conf_log_error(NGX_LOG_DEBUG, cf, 0,
                       "Using %uzKiB of shared memory for upstream_fair",
                       main->shm_size >> 10);
    return NGX_CONF_OK;
}

static char *
ngx_http_upstream_fair(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t *uscf;
    ngx_str_t *value = cf->args->elts;
    ngx_uint_t i, extra_peer_flags = 0;

    for (i = 1; i < cf->args->nelts; i++) {
        if (ngx_strcmp(value[i].data, "no_rr") == 0) {
            extra_peer_flags |= NGX_HTTP_UPSTREAM_FAIR_NO_RR;
        } else if (ngx_strcmp(value[i].data, "weight_mode=peak") == 0) {
            if (extra_peer_flags & NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_MASK) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "weight_mode= options are mutually exclusive");
                return NGX_CONF_ERROR;
            }
            extra_peer_flags |= NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_PEAK;
        } else if (ngx_strcmp(value[i].data, "weight_mode=idle") == 0) {
            if (extra_peer_flags & NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_MASK) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "weight_mode= options are mutually exclusive");
                return NGX_CONF_ERROR;
            }
            extra_peer_flags |= NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_IDLE;
        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "Invalid `fair' parameter `%V'", &value[i]);
            return NGX_CONF_ERROR;
        }
    }

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    if (uscf->peer.init_upstream) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "load balancing method redefined");
    }
    uscf->peer.init_upstream = ngx_http_upstream_init_fair;
    uscf->flags = NGX_HTTP_UPSTREAM_CREATE
                  |NGX_HTTP_UPSTREAM_WEIGHT
                  |NGX_HTTP_UPSTREAM_MAX_CONNS
                  |NGX_HTTP_UPSTREAM_MAX_FAILS
                  |NGX_HTTP_UPSTREAM_FAIL_TIMEOUT
                  |NGX_HTTP_UPSTREAM_DOWN
                  |NGX_HTTP_UPSTREAM_BACKUP
                  |extra_peer_flags;
    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_upstream_init_fair(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_fair_peers_t *peers;

#if (NGX_HTTP_UPSTREAM_ZONE)
    if (us->shm_zone != NULL) {
        /* zone 按 rr 布局复制 peer；fair 的状态由独立共享区持有。 */
        ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0,
                           "upstream \"%V\": \"zone\" is not used by fair, "
                           "peer state is kept in \"upstream_fair\"", &us->host);
        us->shm_zone = NULL;
    }
#endif
    if (ngx_http_upstream_fair_init_peers(cf, us) != NGX_OK) {
        return NGX_ERROR;
    }
    for (peers = us->peer.data; peers; peers = peers->next) {
        peers->no_rr = !!(us->flags & NGX_HTTP_UPSTREAM_FAIR_NO_RR);
        if (us->flags & NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_IDLE) {
            peers->weight_mode = WM_IDLE;
        } else if (us->flags & NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_PEAK) {
            peers->weight_mode = WM_PEAK;
        }
        if (ngx_http_upstream_fair_shm_register(cf, peers) != NGX_OK) {
            return NGX_ERROR;
        }
    }
    us->peer.init = ngx_http_upstream_init_fair_peer;
    return NGX_OK;
}

/* 组锁内同时维护本 worker 贡献与汇总，重生时可以准确重建汇总。 */
static void
ngx_http_upstream_fair_update_nreq(ngx_http_upstream_fair_peer_data_t *fp,
    int delta, ngx_log_t *log)
{
    ngx_uint_t worker = ngx_process == NGX_PROCESS_SINGLE ? 0 : ngx_worker;

    ngx_http_upstream_fair_worker_nreq(fp->peers->shared, worker, fp->current) += delta;
    fp->peers->peer[fp->current].shared->nreq += delta;
    fp->peers->shared->total_nreq += delta;
    ngx_log_debug6(NGX_LOG_DEBUG_HTTP, log, 0,
        "[upstream_fair] nreq for peer %ui @ %p/%p now %ui, total %ui, delta %d",
        fp->current, fp->peers, fp->peers->peer[fp->current].shared,
        fp->peers->peer[fp->current].shared->nreq, fp->peers->shared->total_nreq, delta);
}

/* 低 20 位记录分配历史，其余位为在用数；超出表示范围时饱和。 */
#define SCHED_COUNTER_BITS 20
#define SCHED_NREQ_MAX (((ngx_uint_t) -1) >> SCHED_COUNTER_BITS)
#define SCHED_COUNTER_MAX ((1 << SCHED_COUNTER_BITS) - 1)
#define SCHED_SCORE(nreq,delta) (((nreq) << SCHED_COUNTER_BITS) | (~(delta) & SCHED_COUNTER_MAX))

static ngx_uint_t
ngx_http_upstream_fair_sched_score(ngx_peer_connection_t *pc,
    ngx_http_upstream_fair_peer_data_t *fp, ngx_uint_t n)
{
    ngx_http_upstream_fair_shared_t *fs = fp->peers->peer[n].shared;
    ngx_uint_t req_delta = fp->peers->shared->total_requests - fs->last_req_id;

    if ((ngx_int_t) fs->nreq < 0) {
        fp->negative_peer = n;
        fp->negative_nreq = (ngx_int_t) fs->nreq;
        return SCHED_SCORE(0, req_delta);
    }
    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "[upstream_fair] peer %ui: nreq = %ui, req_delta = %ui",
                   n, fs->nreq, req_delta);
    return SCHED_SCORE(ngx_min(fs->nreq, SCHED_NREQ_MAX),
                       ngx_min(req_delta, SCHED_COUNTER_MAX));
}

/* idle、busy 和最终复核共用资格；扫描阶段只读被动健康状态。 */
static ngx_int_t
ngx_http_upstream_fair_try_peer(ngx_peer_connection_t *pc,
    ngx_http_upstream_fair_peer_data_t *fp, ngx_uint_t peer_id)
{
    ngx_http_upstream_fair_peer_t *peer = &fp->peers->peer[peer_id];

    if (ngx_bitvector_test(fp->tried, peer_id) || peer->down) {
        return NGX_BUSY;
    }
#if (NGX_HTTP_UPSTREAM_CHECK)
    if (ngx_http_upstream_check_peer_down(peer->check_index)) {
        return NGX_BUSY;
    }
#endif
    if (peer->max_conns && peer->shared->nreq >= peer->max_conns) {
        return NGX_BUSY;
    }
    if (!fp->single && peer->max_fails
        && peer->shared->fails >= peer->max_fails
        && ngx_time() - peer->shared->checked <= peer->fail_timeout)
    {
        return NGX_BUSY;
    }
    return NGX_OK;
}

static ngx_uint_t
ngx_http_upstream_choose_fair_peer_idle(ngx_peer_connection_t *pc,
    ngx_http_upstream_fair_peer_data_t *fp)
{
    ngx_uint_t i, n, npeers = fp->peers->number;
    ngx_uint_t weight_mode = fp->peers->weight_mode;
    ngx_uint_t best_idx = NGX_PEER_INVALID, best_nreq = (ngx_uint_t) -1;

    for (i = 0, n = fp->current; i < npeers; i++, n = (n + 1) % npeers) {
        ngx_uint_t nreq = fp->peers->peer[n].shared->nreq;
        ngx_uint_t weight = fp->peers->peer[n].weight;

        if (nreq >= weight || (nreq > 0 && weight_mode != WM_IDLE)) {
            continue;
        }
        if (ngx_http_upstream_fair_try_peer(pc, fp, n) != NGX_OK) {
            continue;
        }
        if (weight_mode != WM_IDLE || !fp->peers->no_rr) {
            best_idx = n;
            break;
        }
        /* idle+no_rr 沿用对稍有负载的按需后端的偏好。 */
        if (best_idx == NGX_PEER_INVALID || nreq) {
            if (best_nreq <= nreq) {
                continue;
            }
            best_idx = n;
            best_nreq = nreq;
        }
    }
    return best_idx;
}

static ngx_uint_t
ngx_http_upstream_choose_fair_peer_busy(ngx_peer_connection_t *pc,
    ngx_http_upstream_fair_peer_data_t *fp)
{
    ngx_uint_t i, n, npeers = fp->peers->number;
    ngx_uint_t best_idx = NGX_PEER_INVALID;
    ngx_uint_t sched_score, best_sched_score = (ngx_uint_t) -1;
    ngx_http_upstream_fair_peer_t *peer;
    ngx_uint_t nreq, weight, mf;

    for (i = 0, n = fp->current; i < npeers; i++, n = (n + 1) % npeers) {
        peer = &fp->peers->peer[n];
        nreq = peer->shared->nreq;
        if (fp->peers->weight_mode == WM_PEAK && nreq >= peer->weight) {
            ngx_log_debug3(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                "[upstream_fair] backend %ui has nreq %ui >= weight %ui in WM_PEAK mode",
                n, nreq, peer->weight);
            continue;
        }
        if (ngx_http_upstream_fair_try_peer(pc, fp, n) != NGX_OK) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                           "[upstream_fair] backend %ui already tried", n);
            continue;
        }
        sched_score = ngx_http_upstream_fair_sched_score(pc, fp, n);
        if (fp->peers->weight_mode == WM_DEFAULT) {
            weight = peer->shared->current_weight;
            mf = peer->max_fails;
            if (mf) {
                /* 冷却后探测保留 fails，达到阈值时折减为零，避免无符号下溢。 */
                weight = weight * (mf - ngx_min(peer->shared->fails, mf)) / mf;
            }
            if (weight > 0) {
                sched_score /= weight;
            }
            ngx_log_debug8(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                "[upstream_fair] bss = %ui, ss = %ui (n = %ui, w = %ui/%ui, f = %ui/%ui, weight = %ui)",
                best_sched_score, sched_score, n, peer->shared->current_weight,
                peer->weight, peer->shared->fails, peer->max_fails, weight);
        }
        if (sched_score <= best_sched_score) {
            best_idx = n;
            best_sched_score = sched_score;
        }
    }
    return best_idx;
}

static ngx_int_t
ngx_http_upstream_choose_fair_peer(ngx_peer_connection_t *pc,
    ngx_http_upstream_fair_peer_data_t *fp, ngx_uint_t *peer_id)
{
    ngx_uint_t best_idx;
    ngx_http_upstream_fair_peer_t *peer;
    time_t now;

    if (fp->single) {
        if (ngx_http_upstream_fair_try_peer(pc, fp, 0) != NGX_OK) {
            return NGX_BUSY;
        }
        *peer_id = 0;
        ngx_bitvector_set(fp->tried, 0);
        return NGX_OK;
    }
    best_idx = ngx_http_upstream_choose_fair_peer_idle(pc, fp);
    if (best_idx != NGX_PEER_INVALID) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "[upstream_fair] peer %ui is idle", best_idx);
    } else {
        best_idx = ngx_http_upstream_choose_fair_peer_busy(pc, fp);
    }
    if (best_idx == NGX_PEER_INVALID) {
        return NGX_BUSY;
    }
    /* 扫描期间主动状态可能变化，最终复核后才记入 tried 和业务计数。 */
    if (ngx_http_upstream_fair_try_peer(pc, fp, best_idx) != NGX_OK) {
        ngx_bitvector_set(fp->tried, best_idx);
        return NGX_AGAIN;
    }
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "[upstream_fair] chose peer %ui", best_idx);
    *peer_id = best_idx;
    ngx_bitvector_set(fp->tried, best_idx);
    peer = &fp->peers->peer[best_idx];
    now = ngx_time();
    if (now - peer->shared->checked > peer->fail_timeout) {
        peer->shared->checked = now;
    }
    if (fp->peers->weight_mode == WM_DEFAULT) {
        if (peer->shared->current_weight-- == 0) {
            peer->shared->current_weight = peer->weight;
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                "[upstream_fair] peer %ui expired weight, reset to %ui", best_idx, peer->weight);
        }
    }
    return NGX_OK;
}

/* 请求级位图和轮转游标属于当前组，统计块已在配置加载时关联。 */
static void
ngx_http_upstream_fair_use_group(ngx_http_upstream_fair_peer_data_t *fp,
    ngx_http_upstream_fair_peers_t *peers)
{
    fp->peers = peers;
    fp->current = peers->current;
    ngx_memzero(fp->tried, fp->bitmap_size);
    ngx_memzero(fp->done, fp->bitmap_size);
    (void) ngx_atomic_fetch_add(&peers->shared->total_requests, 1);
}

static ngx_int_t
ngx_http_upstream_get_fair_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_fair_peer_data_t *fp = data;
    ngx_http_upstream_fair_peer_t *peer;
    ngx_atomic_t *lock;
    ngx_uint_t peer_id, i;
    ngx_int_t ret;

retry_group:
    peer_id = fp->current;
    fp->current = (fp->current + 1) % fp->peers->number;
    fp->negative_peer = NGX_PEER_INVALID;
    lock = &fp->peers->shared->lock;
    ngx_http_upstream_fair_lock(lock);
    ret = NGX_BUSY;
    for (i = 0; i < fp->peers->number; i++) {
        ret = ngx_http_upstream_choose_fair_peer(pc, fp, &peer_id);
        if (ret != NGX_AGAIN) {
            break;
        }
    }
    if (ret == NGX_AGAIN) {
        ret = NGX_BUSY;
    }
    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "[upstream_fair] fp->current = %ui, peer_id = %ui, ret = %i",
                   fp->current, peer_id, ret);
    if (ret == NGX_BUSY) {
        pc->name = fp->peers->name;
        fp->current = NGX_PEER_INVALID;
    } else {
        /* assert(ret == NGX_OK); */
        if (pc->tries) {
            pc->tries--;
        }
        peer = &fp->peers->peer[peer_id];
        fp->current = peer_id;
        if (!fp->peers->no_rr) {
            fp->peers->current = peer_id;
        }
        pc->sockaddr = peer->sockaddr;
        pc->socklen = peer->socklen;
        pc->name = &peer->name;
        peer->shared->last_req_id = fp->peers->shared->total_requests;
        ngx_http_upstream_fair_update_nreq(fp, 1, pc->log);
        peer->shared->total_req++;
    }
    ngx_http_upstream_fair_unlock(lock);
    if (fp->negative_peer != NGX_PEER_INVALID) {
        ngx_log_error(NGX_LOG_WARN, pc->log, 0,
                      "[upstream_fair] upstream %ui has negative nreq (%i)",
                      fp->negative_peer, fp->negative_nreq);
    }
    if (ret == NGX_BUSY && fp->peers->next != NULL) {
        ngx_http_upstream_fair_use_group(fp, fp->peers->next);
        goto retry_group;
    }
    return ret;
}

static void
ngx_http_upstream_free_fair_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    ngx_http_upstream_fair_peer_data_t *fp = data;
    ngx_http_upstream_fair_peer_t *peer;
    ngx_atomic_t *lock;
    ngx_uint_t disabled = 0;

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "[upstream_fair] fp->current = %ui, state = %ui, pc->tries = %ui, pc->data = %p",
                   fp->current, state, pc->tries, pc->data);
    if (fp->current == NGX_PEER_INVALID) {
        return;
    }
    lock = &fp->peers->shared->lock;
    ngx_http_upstream_fair_lock(lock);
    if (ngx_bitvector_test(fp->done, fp->current)) {
        ngx_http_upstream_fair_unlock(lock);
        return;
    }
    ngx_bitvector_set(fp->done, fp->current);
    ngx_http_upstream_fair_update_nreq(fp, -1, pc->log);
    if (fp->single) {
        pc->tries = 0;
    }
    peer = &fp->peers->peer[fp->current];
    if (state & NGX_PEER_FAILED) {
        peer->shared->fails++;
        peer->shared->accessed = peer->shared->checked = ngx_time();
        disabled = !fp->single && peer->max_fails
                   && peer->shared->fails >= peer->max_fails;
    } else if (peer->shared->accessed < peer->shared->checked) {
        peer->shared->fails = 0;
    }
    ngx_http_upstream_fair_unlock(lock);
    if (disabled) {
        ngx_log_error(NGX_LOG_WARN, pc->log, 0, "upstream server temporarily disabled");
    }
}

static ngx_int_t
ngx_http_upstream_init_fair_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_fair_peer_data_t *fp = r->upstream->peer.data;
    ngx_http_upstream_fair_peers_t *usfp = us->peer.data;
    ngx_uint_t n;

    if (fp == NULL) {
        fp = ngx_palloc(r->pool, sizeof(*fp));
        if (fp == NULL) {
            return NGX_ERROR;
        }
        r->upstream->peer.data = fp;
    }
    fp->single = usfp->number == 1 && usfp->next == NULL;
    n = usfp->next ? ngx_max(usfp->number, usfp->next->number) : usfp->number;
    fp->bitmap_size = ((n + NGX_BITVECTOR_ELT_SIZE - 1) / NGX_BITVECTOR_ELT_SIZE)
                      * sizeof(uintptr_t);
    fp->tried = ngx_bitvector_alloc(r->pool, n, &fp->data);
    fp->done = ngx_bitvector_alloc(r->pool, n, &fp->data2);
    if (fp->tried == NULL || fp->done == NULL) {
        return NGX_ERROR;
    }
    ngx_http_upstream_fair_use_group(fp, usfp);
    r->upstream->peer.get = ngx_http_upstream_get_fair_peer;
    r->upstream->peer.free = ngx_http_upstream_free_fair_peer;
    r->upstream->peer.tries = usfp->number + (usfp->next ? usfp->next->number : 0);
#if (NGX_HTTP_SSL)
    r->upstream->peer.set_session = ngx_http_upstream_fair_set_session;
    r->upstream->peer.save_session = ngx_http_upstream_fair_save_session;
#endif
    return NGX_OK;
}

#if (NGX_HTTP_SSL)
static ngx_int_t
ngx_http_upstream_fair_set_session(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_fair_peer_data_t *fp = data;
    ngx_http_upstream_fair_peer_t *peer;
    ngx_ssl_session_t *ssl_session;
    ngx_int_t rc;

    if (fp->current == NGX_PEER_INVALID) {
        return NGX_OK;
    }
    peer = &fp->peers->peer[fp->current];
    /* TODO: threads only mutex */
    /* ngx_lock_mutex(fp->peers->mutex); */
    ssl_session = peer->ssl_session;
    rc = ngx_ssl_set_session(pc->connection, ssl_session);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "set session: %p", ssl_session);
    /* ngx_unlock_mutex(fp->peers->mutex); */
    return rc;
}

static void
ngx_http_upstream_fair_save_session(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_fair_peer_data_t *fp = data;
    ngx_ssl_session_t *old_ssl_session, *ssl_session;
    ngx_http_upstream_fair_peer_t *peer;

    if (fp->current == NGX_PEER_INVALID) {
        return;
    }
    ssl_session = ngx_ssl_get_session(pc->connection);
    if (ssl_session == NULL) {
        return;
    }
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "save session: %p", ssl_session);
    peer = &fp->peers->peer[fp->current];
    /* TODO: threads only mutex */
    /* ngx_lock_mutex(fp->peers->mutex); */
    old_ssl_session = peer->ssl_session;
    peer->ssl_session = ssl_session;
    /* ngx_unlock_mutex(fp->peers->mutex); */
    if (old_ssl_session) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "old session: %p", old_ssl_session);
        /* TODO: may block */
        ngx_ssl_free_session(old_ssl_session);
    }
}
#endif

/* vim: set et ts=4 sw=4: */
