#include "ngx_http_upstream_fair_shm.h"
#include "../peers/ngx_http_upstream_fair_peers.h"

extern ngx_module_t ngx_http_upstream_fair_module;

static ngx_int_t ngx_http_upstream_fair_init_zone(ngx_shm_zone_t *zone,
    void *data);

void *
ngx_http_upstream_fair_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_fair_main_conf_t *main;

    main = ngx_pcalloc(cf->pool, sizeof(*main));
    if (main == NULL) {
        return NULL;
    }
    main->shm_size = ngx_max((size_t) 1024 * 1024, 8 * ngx_pagesize);
    main->cycle = cf->cycle;
    if (ngx_array_init(&main->groups, cf->pool, 4,
                      sizeof(ngx_http_upstream_fair_peers_t *)) != NGX_OK)
    {
        return NULL;
    }
    return main;
}

ngx_int_t
ngx_http_upstream_fair_shm_register(ngx_conf_t *cf,
    ngx_http_upstream_fair_peers_t *peers)
{
    ngx_str_t name = ngx_string("upstream_fair");
    ngx_http_upstream_fair_main_conf_t *main;
    ngx_http_upstream_fair_peers_t **slot;

    main = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_fair_module);
    if (main->zone == NULL) {
        main->zone = ngx_shared_memory_add(cf, &name, main->shm_size,
                                          &ngx_http_upstream_fair_module);
        if (main->zone == NULL) {
            return NGX_ERROR;
        }
        /* 每轮配置单独映射，旧进程排空期间继续持有旧映射。 */
        main->zone->noreuse = 1;
        main->zone->data = main;
        main->zone->init = ngx_http_upstream_fair_init_zone;
    }
    slot = ngx_array_push(&main->groups);
    if (slot == NULL) {
        return NGX_ERROR;
    }
    *slot = peers;
    return NGX_OK;
}

/* 只计算统计分配规模用于报错，配置指定的共享区大小始终是固定值。 */
static ngx_int_t
ngx_http_upstream_fair_block_size(ngx_uint_t number, ngx_uint_t workers,
    size_t *size, size_t *allocation)
{
    size_t unit, header, rounded;

    header = offsetof(ngx_http_upstream_fair_shm_block_t, stats);
    if (workers > (SIZE_MAX - sizeof(ngx_http_upstream_fair_shared_t))
                  / sizeof(ngx_uint_t))
    {
        return NGX_ERROR;
    }
    unit = sizeof(ngx_http_upstream_fair_shared_t)
           + workers * sizeof(ngx_uint_t);
    if (number > (SIZE_MAX - header) / unit) {
        return NGX_ERROR;
    }
    *size = header + number * unit;
    if (*size > ngx_pagesize / 2) {
        if (*size > SIZE_MAX - (ngx_pagesize - 1)) {
            return NGX_ERROR;
        }
        rounded = ngx_align(*size, ngx_pagesize);
    } else {
        for (rounded = 8; rounded < *size; rounded <<= 1) { /* slab 最小块为 8 字节。 */ }
    }
    *allocation = rounded;
    return NGX_OK;
}

static ngx_int_t
ngx_http_upstream_fair_init_zone(ngx_shm_zone_t *zone, void *data)
{
    ngx_http_upstream_fair_main_conf_t *main = zone->data;
    ngx_http_upstream_fair_peers_t **groups = main->groups.elts;
    ngx_http_upstream_fair_shm_block_t *block, **tail;
    ngx_slab_pool_t *slab = (ngx_slab_pool_t *) zone->shm.addr;
    ngx_core_conf_t *core;
    ngx_uint_t workers, i, n;
    size_t size, allocation, total = 0;

    core = (ngx_core_conf_t *) ngx_get_conf(main->cycle->conf_ctx,
                                           ngx_core_module);
    workers = ngx_max(core->worker_processes, 1);
    for (i = 0; i < main->groups.nelts; i++) {
        if (ngx_http_upstream_fair_block_size(groups[i]->number, workers,
                                             &size, &allocation) != NGX_OK
            || allocation > SIZE_MAX - total)
        {
            ngx_log_error(NGX_LOG_EMERG, zone->shm.log, 0,
                          "fair statistics size overflow");
            return NGX_ERROR;
        }
        total += allocation;
    }

    block = slab->data;
    tail = (ngx_http_upstream_fair_shm_block_t **) &slab->data;
    for (i = 0; i < main->groups.nelts; i++) {
        if (zone->shm.exists) {
            /* 非 fork 平台重新解析配置时，按同一登记顺序关联已有块。 */
            if (block == NULL || block->number != groups[i]->number
                || block->workers != workers)
            {
                ngx_log_error(NGX_LOG_EMERG, zone->shm.log, 0,
                              "fair shared statistics layout mismatch");
                return NGX_ERROR;
            }
        } else {
            (void) ngx_http_upstream_fair_block_size(groups[i]->number, workers,
                                                    &size, &allocation);
            block = ngx_slab_calloc(slab, size);
            if (block == NULL) {
                ngx_log_error(NGX_LOG_EMERG, zone->shm.log, 0,
                              "upstream_fair_shm_size too small: current %uz bytes, "
                              "statistics allocations require %uz bytes plus slab "
                              "overhead; increase upstream_fair_shm_size", zone->shm.size, total);
                return NGX_ERROR;
            }
            block->number = groups[i]->number;
            block->workers = workers;
            *tail = block;
            tail = &block->next;
        }
        groups[i]->shared = block;
        for (n = 0; n < block->number; n++) {
            groups[i]->peer[n].shared = &block->stats[n];
        }
        block = block->next;
    }
    return NGX_OK;
}

void
ngx_http_upstream_fair_lock(ngx_atomic_t *lock)
{
    ngx_spinlock(lock, ngx_pid, 1024);
}

void
ngx_http_upstream_fair_unlock(ngx_atomic_t *lock)
{
    (void) ngx_atomic_cmp_set(lock, ngx_pid, 0);
}

/* 启动恢复路径也复核等待中死亡的持有者；请求热路径使用原有自旋锁。 */
static void
ngx_http_upstream_fair_recovery_lock(ngx_atomic_t *lock, ngx_log_t *log)
{
#if !(NGX_WIN32)
    ngx_atomic_uint_t holder;
#endif

    for ( ;; ) {
        if (*lock == 0 && ngx_atomic_cmp_set(lock, 0, ngx_pid)) {
            return;
        }
#if !(NGX_WIN32)
        holder = *lock;
        if (holder != 0 && kill((ngx_pid_t) holder, 0) == -1
            && ngx_errno == NGX_ESRCH
            && ngx_atomic_cmp_set(lock, holder, 0))
        {
            ngx_log_error(NGX_LOG_ALERT, log, 0,
                          "fair recovered group lock held by exited process %P",
                          (ngx_pid_t) holder);
        }
#endif
        ngx_sched_yield();
    }
}

ngx_int_t
ngx_http_upstream_fair_init_process(ngx_cycle_t *cycle)
{
    ngx_http_upstream_fair_main_conf_t *main;
    ngx_http_upstream_fair_peers_t **groups;
    ngx_http_upstream_fair_shm_block_t *block;
    ngx_uint_t i, n, w, worker, reclaimed;

    if (ngx_process != NGX_PROCESS_WORKER && ngx_process != NGX_PROCESS_SINGLE) {
        return NGX_OK;
    }
    main = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_upstream_fair_module);
    if (main == NULL) {
        return NGX_OK;
    }
    worker = ngx_process == NGX_PROCESS_SINGLE ? 0 : ngx_worker;
    groups = main->groups.elts;
    for (i = 0; i < main->groups.nelts; i++) {
        block = groups[i]->shared;
        if (worker >= block->workers) {
            return NGX_ERROR;
        }
        ngx_http_upstream_fair_recovery_lock(&block->lock, cycle->log);
        reclaimed = 0;
        block->total_nreq = 0;
        for (n = 0; n < block->number; n++) {
            reclaimed += ngx_http_upstream_fair_worker_nreq(block, worker, n);
            ngx_http_upstream_fair_worker_nreq(block, worker, n) = 0;
            /* 异常退出可打断多字段更新，以存活 worker 的单元重建汇总值。 */
            block->stats[n].nreq = 0;
            for (w = 0; w < block->workers; w++) {
                block->stats[n].nreq += ngx_http_upstream_fair_worker_nreq(block, w, n);
            }
            block->total_nreq += block->stats[n].nreq;
        }
        ngx_http_upstream_fair_unlock(&block->lock);
        if (reclaimed != 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "fair upstream \"%V\" recovered %ui requests from worker %ui",
                          groups[i]->name, reclaimed, worker);
        }
    }
    return NGX_OK;
}
