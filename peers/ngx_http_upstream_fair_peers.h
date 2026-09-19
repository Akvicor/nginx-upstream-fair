#ifndef NGX_HTTP_UPSTREAM_FAIR_PEERS_H
#define NGX_HTTP_UPSTREAM_FAIR_PEERS_H
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/* 在用计数、被动失败和权重属于实际选中的组及后端。 */
typedef struct {
    ngx_uint_t nreq;
    ngx_uint_t total_req;
    ngx_uint_t last_req_id;
    ngx_uint_t fails;
    ngx_uint_t current_weight;
    time_t accessed;
} ngx_http_upstream_fair_shared_t;

typedef struct {
    ngx_rbtree_node_t node;
    ngx_uint_t generation;
    uintptr_t peers;
    ngx_uint_t total_nreq;
    ngx_uint_t total_requests;
    ngx_atomic_t lock;
    ngx_http_upstream_fair_shared_t stats[1];
} ngx_http_upstream_fair_shm_block_t;

/* 业务地址来自配置池；检查索引随整个 peer 排序。 */
typedef struct {
    ngx_http_upstream_fair_shared_t *shared;
    struct sockaddr *sockaddr;
    socklen_t socklen;
    ngx_str_t name;
    ngx_uint_t weight;
    ngx_uint_t max_fails;
    time_t fail_timeout;
    ngx_uint_t down:1;
#if (NGX_HTTP_UPSTREAM_CHECK)
    ngx_uint_t check_index;
#endif
#if (NGX_HTTP_SSL)
    ngx_ssl_session_t *ssl_session;
#endif
} ngx_http_upstream_fair_peer_t;

/* 主备共享相同调度参数，每组拥有独立游标和按需统计块。 */
typedef struct ngx_http_upstream_fair_peers_s ngx_http_upstream_fair_peers_t;
struct ngx_http_upstream_fair_peers_s {
    ngx_http_upstream_fair_shm_block_t *shared;
    ngx_uint_t current;
    ngx_uint_t size_err:1;
    ngx_uint_t no_rr:1;
    ngx_uint_t weight_mode:2;
    ngx_uint_t number;
    ngx_str_t *name;
    ngx_http_upstream_fair_peers_t *next;
    ngx_http_upstream_fair_peer_t peer[1];
};

/* 根据原生 upstream 配置构造主备、注册检查并完成排序。 */
ngx_int_t ngx_http_upstream_fair_init_peers(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);
#endif
