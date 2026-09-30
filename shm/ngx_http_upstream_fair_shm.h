#ifndef NGX_HTTP_UPSTREAM_FAIR_SHM_H
#define NGX_HTTP_UPSTREAM_FAIR_SHM_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/* 被动健康与调度统计按组加锁，跨 worker 共享。 */
typedef struct {
    ngx_uint_t nreq;
    ngx_uint_t total_req;
    ngx_uint_t last_req_id;
    ngx_uint_t fails;
    ngx_uint_t current_weight;
    time_t accessed;
    time_t checked;
} ngx_http_upstream_fair_shared_t;

/* stats 后紧接 workers × number 个在用计数，供重生 worker 回收自身贡献。 */
typedef struct ngx_http_upstream_fair_shm_block_s
    ngx_http_upstream_fair_shm_block_t;
struct ngx_http_upstream_fair_shm_block_s {
    ngx_http_upstream_fair_shm_block_t *next;
    ngx_uint_t number;
    ngx_uint_t workers;
    ngx_uint_t total_nreq;
    ngx_atomic_t total_requests;
    ngx_atomic_t lock;
    ngx_http_upstream_fair_shared_t stats[1];
};

typedef struct ngx_http_upstream_fair_peers_s ngx_http_upstream_fair_peers_t;

/* 列表与大小随配置周期存活；共享块由本周期独立映射持有。 */
typedef struct {
    size_t shm_size;
    ngx_array_t groups;
    ngx_shm_zone_t *zone;
    ngx_cycle_t *cycle;
} ngx_http_upstream_fair_main_conf_t;

/* 映射中的 worker 单元按 worker 编号、peer 下标排列。 */
#define ngx_http_upstream_fair_worker_nreq(block, worker, peer)                 \
    (((ngx_uint_t *) ((block)->stats + (block)->number))                       \
         [(worker) * (block)->number + (peer)])

/* 创建本周期配置并登记需要在加载期分配统计的组。 */
void *ngx_http_upstream_fair_create_main_conf(ngx_conf_t *cf);
ngx_int_t ngx_http_upstream_fair_shm_register(ngx_conf_t *cf,
    ngx_http_upstream_fair_peers_t *peers);

/* 重生进程按原 worker 编号回收残留，单进程使用编号零。 */
ngx_int_t ngx_http_upstream_fair_init_process(ngx_cycle_t *cycle);

/* 请求路径共用每组的 PID 自旋锁。 */
void ngx_http_upstream_fair_lock(ngx_atomic_t *lock);
void ngx_http_upstream_fair_unlock(ngx_atomic_t *lock);

#endif
