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

/* ngx_spinlock is defined without a matching unlock primitive */
#define ngx_spinlock_unlock(lock)       (void) ngx_atomic_cmp_set(lock, ngx_pid, 0)

#define NGX_HTTP_UPSTREAM_FAIR_NO_RR            (1<<26)
#define NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_IDLE (1<<27)
#define NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_PEAK (1<<28)
#define NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_MASK ((1<<27) | (1<<28))

enum { WM_DEFAULT = 0, WM_IDLE, WM_PEAK };


#define NGX_PEER_INVALID (~0UL)

typedef struct {
    ngx_http_upstream_fair_peers_t     *peers;
    ngx_uint_t                          current;
    uintptr_t                          *tried;
    uintptr_t                          *done;
    uintptr_t                           data;
    uintptr_t                           data2;
    size_t                              bitmap_size;
    unsigned                            single:1;
    unsigned                            has_backup:1;
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
static ngx_int_t ngx_http_upstream_fair_init_module(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_upstream_fair_use_group(
    ngx_http_upstream_fair_peer_data_t *fp, ngx_http_upstream_fair_peers_t *peers,
    ngx_log_t *log);

#if (NGX_HTTP_EXTENDED_STATUS)
static ngx_chain_t *ngx_http_upstream_fair_report_status(ngx_http_request_t *r,
    ngx_int_t *length);
#endif

#if (NGX_HTTP_SSL)
static ngx_int_t ngx_http_upstream_fair_set_session(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_fair_save_session(ngx_peer_connection_t *pc,
    void *data);
#endif

static ngx_command_t  ngx_http_upstream_fair_commands[] = {

    { ngx_string("fair"),
      NGX_HTTP_UPS_CONF|NGX_CONF_ANY,
      ngx_http_upstream_fair,
      0,
      0,
      NULL },

    { ngx_string("upstream_fair_shm_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_fair_set_shm_size,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_upstream_fair_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL,                                  /* merge location configuration */

#if (NGX_HTTP_EXTENDED_STATUS)
    ngx_http_upstream_fair_report_status,
#endif
};


ngx_module_t  ngx_http_upstream_fair_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_fair_module_ctx, /* module context */
    ngx_http_upstream_fair_commands,    /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    ngx_http_upstream_fair_init_module,    /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_uint_t ngx_http_upstream_fair_shm_size;
static ngx_shm_zone_t * ngx_http_upstream_fair_shm_zone;
static ngx_rbtree_t * ngx_http_upstream_fair_rbtree;
static ngx_uint_t ngx_http_upstream_fair_generation;

static int
ngx_http_upstream_fair_compare_rbtree_node(const ngx_rbtree_node_t *v_left,
    const ngx_rbtree_node_t *v_right)
{
    ngx_http_upstream_fair_shm_block_t *left, *right;

    left = (ngx_http_upstream_fair_shm_block_t *) v_left;
    right = (ngx_http_upstream_fair_shm_block_t *) v_right;

    if (left->generation < right->generation) {
        return -1;
    } else if (left->generation > right->generation) {
        return 1;
    } else { /* left->generation == right->generation */
        if (left->peers < right->peers) {
            return -1;
        } else if (left->peers > right->peers) {
            return 1;
        } else {
            return 0;
        }
    }
}

/*
 * generic functions start here
 */
static void
ngx_rbtree_generic_insert(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel,
    int (*compare)(const ngx_rbtree_node_t *left, const ngx_rbtree_node_t *right))
{
    for ( ;; ) {
        if (node->key < temp->key) {

            if (temp->left == sentinel) {
                temp->left = node;
                break;
            }

            temp = temp->left;

        } else if (node->key > temp->key) {

            if (temp->right == sentinel) {
                temp->right = node;
                break;
            }

            temp = temp->right;

        } else { /* node->key == temp->key */
            if (compare(node, temp) < 0) {

                if (temp->left == sentinel) {
                    temp->left = node;
                    break;
                }

                temp = temp->left;

            } else {

                if (temp->right == sentinel) {
                    temp->right = node;
                    break;
                }

                temp = temp->right;
            }
        }
    }

    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}

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
    ngx_uint_t                      n, m;

    n = bit / NGX_BITVECTOR_ELT_SIZE;
    m = (uintptr_t) 1 << (bit % NGX_BITVECTOR_ELT_SIZE);

    return bv[n] & m;
}

static void
ngx_bitvector_set(uintptr_t *bv, ngx_uint_t bit)
{
    ngx_uint_t                      n, m;

    n = bit / NGX_BITVECTOR_ELT_SIZE;
    m = (uintptr_t) 1 << (bit % NGX_BITVECTOR_ELT_SIZE);

    bv[n] |= m;
}

/*
 * generic functions end here
 */

static ngx_int_t
ngx_http_upstream_fair_init_module(ngx_cycle_t *cycle)
{
    ngx_list_part_t *part;
    ngx_shm_zone_t *zones;
    ngx_uint_t i;

    /* 原生配置提交后才发布共享区入口，失败候选配置不会污染后续重生进程。 */
    ngx_http_upstream_fair_shm_zone = NULL;
    ngx_http_upstream_fair_rbtree = NULL;
    for (part = &cycle->shared_memory.part; part; part = part->next) {
        zones = part->elts;
        for (i = 0; i < part->nelts; i++) {
            if (zones[i].tag == &ngx_http_upstream_fair_module) {
                ngx_http_upstream_fair_shm_zone = &zones[i];
                ngx_http_upstream_fair_rbtree = zones[i].data;
            }
        }
    }
    ngx_http_upstream_fair_generation++;
    return NGX_OK;
}

static void
ngx_http_upstream_fair_rbtree_insert(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel) {

    ngx_rbtree_generic_insert(temp, node, sentinel,
        ngx_http_upstream_fair_compare_rbtree_node);
}


static ngx_int_t
ngx_http_upstream_fair_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_slab_pool_t                *shpool;
    ngx_rbtree_t                   *tree;
    ngx_rbtree_node_t              *sentinel;

    if (data) {
        shm_zone->data = data;
        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    tree = ngx_slab_alloc(shpool, sizeof *tree);
    if (tree == NULL) {
        return NGX_ERROR;
    }

    sentinel = ngx_slab_alloc(shpool, sizeof *sentinel);
    if (sentinel == NULL) {
        return NGX_ERROR;
    }

    ngx_rbtree_sentinel_init(sentinel);
    tree->root = sentinel;
    tree->sentinel = sentinel;
    tree->insert = ngx_http_upstream_fair_rbtree_insert;
    shm_zone->data = tree;

    return NGX_OK;
}


static char *
ngx_http_upstream_fair_set_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ssize_t                         new_shm_size;
    ngx_str_t                      *value;

    value = cf->args->elts;

    new_shm_size = ngx_parse_size(&value[1]);
    if (new_shm_size == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Invalid memory area size `%V'", &value[1]);
        return NGX_CONF_ERROR;
    }

    new_shm_size = ngx_align(new_shm_size, ngx_pagesize);

    if (new_shm_size < 8 * (ssize_t) ngx_pagesize) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "The upstream_fair_shm_size value must be at least %udKiB", (8 * ngx_pagesize) >> 10);
        new_shm_size = 8 * ngx_pagesize;
    }

    if (ngx_http_upstream_fair_shm_size &&
        ngx_http_upstream_fair_shm_size != (ngx_uint_t) new_shm_size) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "Cannot change memory area size without restart, ignoring change");
    } else {
        ngx_http_upstream_fair_shm_size = new_shm_size;
    }
    ngx_conf_log_error(NGX_LOG_DEBUG, cf, 0, "Using %udKiB of shared memory for upstream_fair", new_shm_size >> 10);

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_fair(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_uint_t i;
    ngx_uint_t extra_peer_flags = 0;

    for (i = 1; i < cf->args->nelts; i++) {
        ngx_str_t *value = cf->args->elts;
        if (ngx_strcmp(value[i].data, "no_rr") == 0) {
            extra_peer_flags |= NGX_HTTP_UPSTREAM_FAIR_NO_RR;
        } else if (ngx_strcmp(value[i].data, "weight_mode=peak") == 0) {
            if (extra_peer_flags & NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_MASK) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "weight_mode= options are mutually exclusive");
                return NGX_CONF_ERROR;
            }
            extra_peer_flags |= NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_PEAK;
        } else if (ngx_strcmp(value[i].data, "weight_mode=idle") == 0) {
            if (extra_peer_flags & NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_MASK) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "weight_mode= options are mutually exclusive");
                return NGX_CONF_ERROR;
            }
            extra_peer_flags |= NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_IDLE;
        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Invalid `fair' parameter `%V'", &value[i]);
            return NGX_CONF_ERROR;
        }
    }

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    uscf->peer.init_upstream = ngx_http_upstream_init_fair;

    uscf->flags = NGX_HTTP_UPSTREAM_CREATE
                  |NGX_HTTP_UPSTREAM_WEIGHT
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
    ngx_http_upstream_fair_peers_t     *peers;
    ngx_str_t                          *shm_name;
    ngx_shm_zone_t                     *zone;

    if (ngx_http_upstream_fair_init_peers(cf, us) != NGX_OK) {
        return NGX_ERROR;
    }

    peers = us->peer.data;

    shm_name = ngx_palloc(cf->pool, sizeof *shm_name);
    if (shm_name == NULL) {
        return NGX_ERROR;
    }
    shm_name->len = sizeof("upstream_fair") - 1;
    shm_name->data = (unsigned char *) "upstream_fair";

    if (ngx_http_upstream_fair_shm_size == 0) {
        ngx_http_upstream_fair_shm_size = 8 * ngx_pagesize;
    }

    zone = ngx_shared_memory_add(
        cf, shm_name, ngx_http_upstream_fair_shm_size, &ngx_http_upstream_fair_module);
    if (zone == NULL) {
        return NGX_ERROR;
    }
    zone->init = ngx_http_upstream_fair_init_shm_zone;

    for ( ; peers; peers = peers->next) {
        peers->no_rr = !!(us->flags & NGX_HTTP_UPSTREAM_FAIR_NO_RR);
        if (us->flags & NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_IDLE) {
            peers->weight_mode = WM_IDLE;
        } else if (us->flags & NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_PEAK) {
            peers->weight_mode = WM_PEAK;
        }
    }

    us->peer.init = ngx_http_upstream_init_fair_peer;

    return NGX_OK;
}


static void
ngx_http_upstream_fair_update_nreq(ngx_http_upstream_fair_peer_data_t *fp, int delta, ngx_log_t *log)
{
#if (NGX_DEBUG)
    ngx_uint_t                          nreq;
    ngx_uint_t                          total_nreq;

#endif
    /* 计数参与忙闲/容量选择和旧块存活判断，始终在现有组锁内更新。 */
    fp->peers->peer[fp->current].shared->nreq += delta;
    fp->peers->shared->total_nreq += delta;
#if (NGX_DEBUG)
    nreq = fp->peers->peer[fp->current].shared->nreq;
    total_nreq = fp->peers->shared->total_nreq;

    ngx_log_debug6(NGX_LOG_DEBUG_HTTP, log, 0,
        "[upstream_fair] nreq for peer %ui @ %p/%p now %d, total %d, delta %d",
        fp->current, fp->peers, fp->peers->peer[fp->current].shared, nreq,
        total_nreq, delta);
#endif
}

/*
 * SCHED_COUNTER_BITS is the portion of an ngx_uint_t which represents
 * the req_delta part (number of requests serviced on _other_
 * backends). The rest (top bits) represents the number of currently
 * processed requests.
 *
 * The value is not too critical because overflow is handled via
 * saturation. With the default value of 20, scheduling is exact for
 * fewer than 4k concurrent requests per backend (on 32-bit
 * architectures) and fewer than 1M concurrent requests to all backends
 * together. Beyond these limits, the algorithm essentially falls back
 * to pure weighted round-robin.
 *
 * A higher score means less suitable.
 *
 * The `delta' parameter is bit-negated so that high values yield low
 * scores and get chosen more often.
 */

#define SCHED_COUNTER_BITS 20
#define SCHED_NREQ_MAX ((~0UL) >> SCHED_COUNTER_BITS)
#define SCHED_COUNTER_MAX ((1 << SCHED_COUNTER_BITS) - 1)
#define SCHED_SCORE(nreq,delta) (((nreq) << SCHED_COUNTER_BITS) | (~(delta) & SCHED_COUNTER_MAX))
#define ngx_upstream_fair_min(a,b) (((a) < (b)) ? (a) : (b))

static ngx_uint_t
ngx_http_upstream_fair_sched_score(ngx_peer_connection_t *pc,
    ngx_http_upstream_fair_peer_data_t *fp,
    ngx_uint_t n)
{
    ngx_http_upstream_fair_peer_t      *peer = &fp->peers->peer[n];
    ngx_http_upstream_fair_shared_t    *fs = peer->shared;
    ngx_uint_t req_delta = fp->peers->shared->total_requests - fs->last_req_id;

    /* sanity check */
    if ((ngx_int_t)fs->nreq < 0) {
        ngx_log_error(NGX_LOG_WARN, pc->log, 0, "[upstream_fair] upstream %ui has negative nreq (%i)", n, fs->nreq);
        return SCHED_SCORE(0, req_delta);
    }

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] peer %ui: nreq = %i, req_delta = %ui", n, fs->nreq, req_delta);

    return SCHED_SCORE(
        ngx_upstream_fair_min(fs->nreq, SCHED_NREQ_MAX),
        ngx_upstream_fair_min(req_delta, SCHED_COUNTER_MAX));
}

/*
 * the core of load balancing logic
 */

static ngx_int_t
ngx_http_upstream_fair_try_peer(ngx_peer_connection_t *pc,
    ngx_http_upstream_fair_peer_data_t *fp,
    ngx_uint_t peer_id)
{
    ngx_http_upstream_fair_peer_t        *peer;

    if (ngx_bitvector_test(fp->tried, peer_id))
        return NGX_BUSY;

    peer = &fp->peers->peer[peer_id];

#if (NGX_HTTP_UPSTREAM_CHECK)
    if (ngx_http_upstream_check_peer_down(peer->check_index)) {
        return NGX_BUSY;
    }
#endif

    if (!peer->down) {
        if (fp->single || peer->max_fails == 0 || peer->shared->fails < peer->max_fails) {
            return NGX_OK;
        }

        if (ngx_time() - peer->shared->accessed > peer->fail_timeout) {
            ngx_log_debug3(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] resetting fail count for peer %d, time delta %d > %d",
                peer_id, ngx_time() - peer->shared->accessed, peer->fail_timeout);
            peer->shared->fails = 0;
            return NGX_OK;
        }
    }

    return NGX_BUSY;
}

static ngx_uint_t
ngx_http_upstream_choose_fair_peer_idle(ngx_peer_connection_t *pc,
    ngx_http_upstream_fair_peer_data_t *fp)
{
    ngx_uint_t                          i, n;
    ngx_uint_t                          npeers = fp->peers->number;
    ngx_uint_t                          weight_mode = fp->peers->weight_mode;
    ngx_uint_t                          best_idx = NGX_PEER_INVALID;
    ngx_uint_t                          best_nreq = ~0U;

    for (i = 0, n = fp->current; i < npeers; i++, n = (n + 1) % npeers) {
        ngx_uint_t nreq = fp->peers->peer[n].shared->nreq;
        ngx_uint_t weight = fp->peers->peer[n].weight;

        if (fp->peers->peer[n].shared->fails > 0)
            continue;

        if (nreq >= weight || (nreq > 0 && weight_mode != WM_IDLE)) {
            continue;
        }

        if (ngx_http_upstream_fair_try_peer(pc, fp, n) != NGX_OK) {
            continue;
        }

        /* not in WM_IDLE+no_rr mode: the first completely idle backend gets chosen */
        if (weight_mode != WM_IDLE || !fp->peers->no_rr) {
            best_idx = n;
            break;
        }

        /* in WM_IDLE+no_rr mode we actually prefer slightly loaded backends
         * to totally idle ones, under the assumption that they're spawned
         * on demand and can handle up to 'weight' concurrent requests
         */
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

static ngx_int_t
ngx_http_upstream_choose_fair_peer_busy(ngx_peer_connection_t *pc,
    ngx_http_upstream_fair_peer_data_t *fp)
{
    ngx_uint_t                          i, n;
    ngx_uint_t                          npeers = fp->peers->number;
    ngx_uint_t                          weight_mode = fp->peers->weight_mode;
    ngx_uint_t                          best_idx = NGX_PEER_INVALID;
    ngx_uint_t                          sched_score;
    ngx_uint_t                          best_sched_score = ~0UL;

    /*
     * calculate sched scores for all the peers, choosing the lowest one
     */
    for (i = 0, n = fp->current; i < npeers; i++, n = (n + 1) % npeers) {
        ngx_http_upstream_fair_peer_t      *peer;
        ngx_uint_t                          nreq;
        ngx_uint_t                          weight;

        peer = &fp->peers->peer[n];
        nreq = fp->peers->peer[n].shared->nreq;

        if (weight_mode == WM_PEAK && nreq >= peer->weight) {
            ngx_log_debug3(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] backend %d has nreq %ui >= weight %ui in WM_PEAK mode", n, nreq, peer->weight);
            continue;
        }

        if (ngx_http_upstream_fair_try_peer(pc, fp, n) != NGX_OK) {
            if (!pc->tries) {
                ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] all backends exhausted");
                return NGX_PEER_INVALID;
            }

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] backend %d already tried", n);
            continue;
        }

        sched_score = ngx_http_upstream_fair_sched_score(pc, fp, n);

        if (weight_mode == WM_DEFAULT) {
            /*
             * take peer weight into account
             */
            weight = peer->shared->current_weight;
            if (peer->max_fails) {
                ngx_uint_t mf = peer->max_fails;
                weight = peer->shared->current_weight * (mf - peer->shared->fails) / mf;
            }
            if (weight > 0) {
                sched_score /= weight;
            }
            ngx_log_debug8(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] bss = %ui, ss = %ui (n = %d, w = %d/%d, f = %d/%d, weight = %d)",
                best_sched_score, sched_score, n, peer->shared->current_weight, peer->weight, peer->shared->fails, peer->max_fails, weight);
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
    ngx_uint_t                          best_idx = NGX_PEER_INVALID;
    ngx_uint_t                          weight_mode;

    weight_mode = fp->peers->weight_mode;

    /* just a single backend */
    if (fp->single) {
        if (ngx_http_upstream_fair_try_peer(pc, fp, 0) != NGX_OK) {
            return NGX_BUSY;
        }
        *peer_id = 0;
        ngx_bitvector_set(fp->tried, 0);
        return NGX_OK;
    }

    /* any idle backends? */
    best_idx = ngx_http_upstream_choose_fair_peer_idle(pc, fp);
    if (best_idx != NGX_PEER_INVALID) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] peer %i is idle", best_idx);
        goto chosen;
    }

    /* no idle backends, choose the least loaded one */
    best_idx = ngx_http_upstream_choose_fair_peer_busy(pc, fp);
    if (best_idx != NGX_PEER_INVALID) {
        goto chosen;
    }

    return NGX_BUSY;

chosen:
    /* 扫描期间主动状态可能变化，最终复核后才记入 tried 和业务计数。 */
    if (ngx_http_upstream_fair_try_peer(pc, fp, best_idx) != NGX_OK) {
        ngx_bitvector_set(fp->tried, best_idx);
        return NGX_AGAIN;
    }
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] chose peer %i", best_idx);
    *peer_id = best_idx;
    ngx_bitvector_set(fp->tried, best_idx);

    if (weight_mode == WM_DEFAULT) {
        ngx_http_upstream_fair_peer_t      *peer = &fp->peers->peer[best_idx];

        if (peer->shared->current_weight-- == 0) {
            peer->shared->current_weight = peer->weight;
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] peer %d expired weight, reset to %d", best_idx, peer->weight);
        }
    }
    return NGX_OK;
}

ngx_int_t
ngx_http_upstream_get_fair_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_int_t                           ret;
    ngx_uint_t                          peer_id, i;
    ngx_http_upstream_fair_peer_data_t *fp = data;
    ngx_http_upstream_fair_peer_t      *peer;
    ngx_atomic_t                       *lock;

retry_group:
    peer_id = fp->current;
    fp->current = (fp->current + 1) % fp->peers->number;

    lock = &fp->peers->shared->lock;
    ngx_spinlock(lock, ngx_pid, 1024);
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
    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] fp->current = %d, peer_id = %d, ret = %d",
        fp->current, peer_id, ret);

    if (ret == NGX_BUSY) {
        if (!fp->has_backup) {
            for (i = 0; i < fp->peers->number; i++) {
                fp->peers->peer[i].shared->fails = 0;
            }
            if (pc->tries) {
                pc->tries--;
            }
        }

        pc->name = fp->peers->name;
        fp->current = NGX_PEER_INVALID;
        ngx_spinlock_unlock(lock);
        if (fp->peers->next != NULL) {
            if (ngx_http_upstream_fair_use_group(fp, fp->peers->next, pc->log) != NGX_OK) {
                return NGX_ERROR;
            }
            goto retry_group;
        }
        return NGX_BUSY;
    }

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
    ngx_spinlock_unlock(lock);
    return ret;
}


void
ngx_http_upstream_free_fair_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    ngx_http_upstream_fair_peer_data_t     *fp = data;
    ngx_http_upstream_fair_peer_t          *peer;
    ngx_atomic_t                           *lock;

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[upstream_fair] fp->current = %d, state = %ui, pc->tries = %d, pc->data = %p",
        fp->current, state, pc->tries, pc->data);

    if (fp->current == NGX_PEER_INVALID) {
        return;
    }

    lock = &fp->peers->shared->lock;
    ngx_spinlock(lock, ngx_pid, 1024);
    if (ngx_bitvector_test(fp->done, fp->current)) {
        ngx_spinlock_unlock(lock);
        return;
    }
    ngx_bitvector_set(fp->done, fp->current);
    ngx_http_upstream_fair_update_nreq(fp, -1, pc->log);

    if (fp->single) {
        pc->tries = 0;
    }

    if (state & NGX_PEER_FAILED) {
        peer = &fp->peers->peer[fp->current];

        peer->shared->fails++;
        peer->shared->accessed = ngx_time();
    }
    ngx_spinlock_unlock(lock);
}

/*
 * walk through the rbtree, removing old entries and looking for
 * a matching one -- compared by (cycle, peers) pair
 *
 * no attempt at optimisation is made, for two reasons:
 *  - the tree will be quite small, anyway
 *  - being called once per worker startup per upstream block,
 *    this code isn't really the hot path
 */
static ngx_http_upstream_fair_shm_block_t *
ngx_http_upstream_fair_walk_shm(
    ngx_slab_pool_t *shpool,
    ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel,
    ngx_http_upstream_fair_peers_t *peers)
{
    ngx_http_upstream_fair_shm_block_t     *uf_node;
    ngx_http_upstream_fair_shm_block_t     *found_node = NULL;
    ngx_http_upstream_fair_shm_block_t     *tmp_node;

    if (node == sentinel) {
        return NULL;
    }

    /* visit left node */
    if (node->left != sentinel) {
        tmp_node = ngx_http_upstream_fair_walk_shm(shpool, node->left,
            sentinel, peers);
        if (tmp_node) {
            found_node = tmp_node;
        }
    }

    /* visit right node */
    if (node->right != sentinel) {
        tmp_node = ngx_http_upstream_fair_walk_shm(shpool, node->right,
            sentinel, peers);
        if (tmp_node) {
            found_node = tmp_node;
        }
    }

    /* visit current node */
    uf_node = (ngx_http_upstream_fair_shm_block_t *) node;
    if (uf_node->generation != ngx_http_upstream_fair_generation) {
        ngx_spinlock(&uf_node->lock, ngx_pid, 1024);
        if (uf_node->total_nreq == 0) {
            /* don't bother unlocking */
            ngx_rbtree_delete(ngx_http_upstream_fair_rbtree, node);
            ngx_slab_free_locked(shpool, node);
        }
        ngx_spinlock_unlock(&uf_node->lock);
    } else if (uf_node->peers == (uintptr_t) peers) {
        found_node = uf_node;
    }

    return found_node;
}

static ngx_int_t
ngx_http_upstream_fair_shm_alloc(ngx_http_upstream_fair_peers_t *usfp, ngx_log_t *log)
{
    ngx_slab_pool_t                        *shpool;
    ngx_uint_t                              i;
    size_t                                  size;

    if (usfp->shared) {
        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *)ngx_http_upstream_fair_shm_zone->shm.addr;

    ngx_shmtx_lock(&shpool->mutex);

    usfp->shared = ngx_http_upstream_fair_walk_shm(shpool,
        ngx_http_upstream_fair_rbtree->root,
        ngx_http_upstream_fair_rbtree->sentinel,
        usfp);

    if (usfp->shared) {
        ngx_shmtx_unlock(&shpool->mutex);
        return NGX_OK;
    }

    size = offsetof(ngx_http_upstream_fair_shm_block_t, stats)
           + usfp->number * sizeof(ngx_http_upstream_fair_shared_t);
    usfp->shared = ngx_slab_alloc_locked(shpool, size);

    if (!usfp->shared) {
        ngx_shmtx_unlock(&shpool->mutex);
        if (!usfp->size_err) {
            ngx_log_error(NGX_LOG_EMERG, log, 0,
                "upstream_fair_shm_size too small (current value is %udKiB)",
                ngx_http_upstream_fair_shm_size >> 10);
            usfp->size_err = 1;
        }
        return NGX_ERROR;
    }

    /* slab 可复用旧组的块，新组的锁和全部统计从确定的零状态开始。 */
    ngx_memzero(usfp->shared, size);
    usfp->shared->node.key = ngx_crc32_short((u_char *) &ngx_cycle, sizeof ngx_cycle) ^
        ngx_crc32_short((u_char *) &usfp, sizeof(usfp));

    usfp->shared->generation = ngx_http_upstream_fair_generation;
    usfp->shared->peers = (uintptr_t) usfp;
    usfp->shared->total_nreq = 0;
    usfp->shared->total_requests = 0;

    for (i = 0; i < usfp->number; i++) {
            usfp->shared->stats[i].nreq = 0;
            usfp->shared->stats[i].last_req_id = 0;
            usfp->shared->stats[i].total_req = 0;
    }

    ngx_rbtree_insert(ngx_http_upstream_fair_rbtree, &usfp->shared->node);

    ngx_shmtx_unlock(&shpool->mutex);
    return NGX_OK;
}

/* 每次请求首次进入一组时建立位图、统计和游标，分配失败返回原生错误。 */
static ngx_int_t
ngx_http_upstream_fair_use_group(ngx_http_upstream_fair_peer_data_t *fp,
    ngx_http_upstream_fair_peers_t *peers, ngx_log_t *log)
{
    ngx_uint_t n;

    if (ngx_http_upstream_fair_shm_alloc(peers, log) != NGX_OK) {
        return NGX_ERROR;
    }
    fp->peers = peers;
    fp->current = peers->current;
    ngx_memzero(fp->tried, fp->bitmap_size);
    ngx_memzero(fp->done, fp->bitmap_size);
    peers->shared->total_requests++;
    for (n = 0; n < peers->number; n++) {
        peers->peer[n].shared = &peers->shared->stats[n];
    }
    return NGX_OK;
}

ngx_int_t
ngx_http_upstream_init_fair_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_fair_peer_data_t     *fp;
    ngx_http_upstream_fair_peers_t         *usfp;
    ngx_uint_t                              n;

    fp = r->upstream->peer.data;

    if (fp == NULL) {
        fp = ngx_palloc(r->pool, sizeof(ngx_http_upstream_fair_peer_data_t));
        if (fp == NULL) {
            return NGX_ERROR;
        }

        r->upstream->peer.data = fp;
    }

    usfp = us->peer.data;

    fp->has_backup = usfp->next != NULL;
    fp->single = usfp->number == 1 && !fp->has_backup;
    n = usfp->next ? ngx_max(usfp->number, usfp->next->number) : usfp->number;
    fp->bitmap_size = ((n + NGX_BITVECTOR_ELT_SIZE - 1) / NGX_BITVECTOR_ELT_SIZE) * sizeof(uintptr_t);
    fp->tried = ngx_bitvector_alloc(r->pool, n, &fp->data);
    fp->done = ngx_bitvector_alloc(r->pool, n, &fp->data2);

    if (fp->tried == NULL || fp->done == NULL) {
        return NGX_ERROR;
    }

    /* set up shared memory area */
    if (ngx_http_upstream_fair_use_group(fp, usfp, r->connection->log) != NGX_OK) {
        return NGX_ERROR;
    }

    r->upstream->peer.get = ngx_http_upstream_get_fair_peer;
    r->upstream->peer.free = ngx_http_upstream_free_fair_peer;
    r->upstream->peer.tries = usfp->number + (usfp->next ? usfp->next->number : 0);
#if (NGX_HTTP_SSL)
    r->upstream->peer.set_session =
                               ngx_http_upstream_fair_set_session;
    r->upstream->peer.save_session =
                               ngx_http_upstream_fair_save_session;
#endif

    return NGX_OK;
}

#if (NGX_HTTP_SSL)
static ngx_int_t
ngx_http_upstream_fair_set_session(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_fair_peer_data_t  *fp = data;

    ngx_int_t                      rc;
    ngx_ssl_session_t             *ssl_session;
    ngx_http_upstream_fair_peer_t *peer;

    if (fp->current == NGX_PEER_INVALID)
        return NGX_OK;

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
    ngx_http_upstream_fair_peer_data_t  *fp = data;

    ngx_ssl_session_t             *old_ssl_session, *ssl_session;
    ngx_http_upstream_fair_peer_t *peer;

    if (fp->current == NGX_PEER_INVALID)
        return;

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

#if (NGX_HTTP_EXTENDED_STATUS)
static void
ngx_http_upstream_fair_walk_status(ngx_pool_t *pool, ngx_chain_t *cl, ngx_int_t *length,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_http_upstream_fair_shm_block_t     *s_node = (ngx_http_upstream_fair_shm_block_t *) node;
    ngx_http_upstream_fair_peers_t         *peers;
    ngx_chain_t                            *new_cl;
    ngx_buf_t                              *b;
    ngx_uint_t                              size, i;

    if (node == sentinel) {
        return;
    }

    if (node->left != sentinel) {
        ngx_http_upstream_fair_walk_status(pool, cl, length, node->left, sentinel);
    }

    if (s_node->generation != ngx_http_upstream_fair_generation) {
        size = 100;
        peers = NULL;
    } else {
        /* this is rather ugly (casting an uintptr_t back into a pointer
         * but as long as the generation is still the same (verified above),
         * it should be still safe
         */
        peers = (ngx_http_upstream_fair_peers_t *) s_node->peers;
        if (!peers->shared) {
            goto next;
        }

        size = 200 + peers->number * 120; /* LOTS of slack */
    }

    b = ngx_create_temp_buf(pool, size);
    if (!b) {
        goto next;
    }

    new_cl = ngx_alloc_chain_link(pool);
    if (!new_cl) {
        goto next;
    }

    new_cl->buf = b;
    new_cl->next = NULL;

    while (cl->next) {
        cl = cl->next;
    }
    cl->next = new_cl;

    if (peers) {
        b->last = ngx_sprintf(b->last, "upstream %V (%p): current peer %d/%d, total requests: %ui\n", peers->name, (void*) node, peers->current, peers->number, s_node->total_requests);
        for (i = 0; i < peers->number; i++) {
            ngx_http_upstream_fair_peer_t *peer = &peers->peer[i];
            ngx_http_upstream_fair_shared_t *sh = peer->shared;
            b->last = ngx_sprintf(b->last, " peer %d: %V weight: %d/%d, fails: %d/%d, acc: %d, down: %d, nreq: %d, total_req: %ui, last_req: %ui\n",
                i, &peer->name, sh->current_weight, peer->weight, sh->fails, peer->max_fails, sh->accessed, peer->down,
                sh->nreq, sh->total_req, sh->last_req_id);
        }
    } else {
        b->last = ngx_sprintf(b->last, "upstream %p: gen %ui != %ui, total_nreq = %ui", (void*) node, s_node->generation, ngx_http_upstream_fair_generation, s_node->total_nreq);
    }
    b->last = ngx_sprintf(b->last, "\n");
    b->last_buf = 1;

    *length += b->last - b->pos;

    if (cl->buf) {
        cl->buf->last_buf = 0;
    }

    cl = cl->next;
next:

    if (node->right != sentinel) {
        ngx_http_upstream_fair_walk_status(pool, cl, length, node->right, sentinel);
    }
}

static ngx_chain_t*
ngx_http_upstream_fair_report_status(ngx_http_request_t *r, ngx_int_t *length)
{
    ngx_buf_t              *b;
    ngx_chain_t            *cl;
    ngx_slab_pool_t        *shpool;

    b = ngx_create_temp_buf(r->pool, sizeof("\nupstream_fair status report:\n"));
    if (!b) {
        return NULL;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (!cl) {
        return NULL;
    }
    cl->next = NULL;
    cl->buf = b;

    b->last = ngx_cpymem(b->last, "\nupstream_fair status report:\n",
        sizeof("\nupstream_fair status report:\n") - 1);

    *length = b->last - b->pos;

    shpool = (ngx_slab_pool_t *)ngx_http_upstream_fair_shm_zone->shm.addr;

    ngx_shmtx_lock(&shpool->mutex);

    ngx_http_upstream_fair_walk_status(r->pool, cl,
        length,
        ngx_http_upstream_fair_rbtree->root,
        ngx_http_upstream_fair_rbtree->sentinel);

    ngx_shmtx_unlock(&shpool->mutex);

    if (!cl->next || !cl->next->buf) {
        /* no upstream_fair status to report */
        return NULL;
    }

    return cl;
}
#endif

/* vim: set et ts=4 sw=4: */
