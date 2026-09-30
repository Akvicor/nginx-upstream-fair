#ifndef NGX_HTTP_UPSTREAM_FAIR_PEERS_H
#define NGX_HTTP_UPSTREAM_FAIR_PEERS_H
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "../shm/ngx_http_upstream_fair_shm.h"

/* 业务地址来自配置池；检查索引随整个 peer 排序。 */
typedef struct {
    ngx_http_upstream_fair_shared_t *shared;
    struct sockaddr *sockaddr;
    socklen_t socklen;
    ngx_str_t name;
    ngx_uint_t weight;
    ngx_uint_t max_fails;
    ngx_uint_t max_conns;
    time_t fail_timeout;
    ngx_uint_t down:1;
#if (NGX_HTTP_UPSTREAM_CHECK)
    ngx_uint_t check_index;
#endif
#if (NGX_HTTP_SSL)
    ngx_ssl_session_t *ssl_session;
#endif
} ngx_http_upstream_fair_peer_t;

/* 主备共享相同调度参数，每组拥有独立游标与加载期分配的统计块。 */
struct ngx_http_upstream_fair_peers_s {
    ngx_http_upstream_fair_shm_block_t *shared;
    ngx_uint_t current;
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
