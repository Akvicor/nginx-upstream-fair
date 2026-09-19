/* Copyright (C) 2007 Grzegorz Nosek */
#include "ngx_http_upstream_fair_peers.h"
#if (NGX_HTTP_UPSTREAM_CHECK)
#include "ngx_http_upstream_check_module.h"
#endif

static ngx_int_t
ngx_http_upstream_fair_compare(const void *one, const void *two)
{
    const ngx_http_upstream_fair_peer_t *first = one, *second = two;
    return first->weight < second->weight;
}

/* 分配前检查零主组及大小溢出，避免可变尾数组的 n-1 下溢。 */
static ngx_http_upstream_fair_peers_t *
ngx_http_upstream_fair_group(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us,
    ngx_uint_t count)
{
    ngx_http_upstream_fair_peers_t *peers;
    size_t size = offsetof(ngx_http_upstream_fair_peers_t, peer);

    if (count == 0 || count > (SIZE_MAX - size) / sizeof(peers->peer[0])) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid fair peer count in upstream \"%V\"", &us->host);
        return NULL;
    }
    peers = ngx_pcalloc(cf->pool, size + count * sizeof(peers->peer[0]));
    if (peers != NULL) {
        peers->number = count;
        peers->name = &us->host;
        peers->current = count - 1;
    }
    return peers;
}

ngx_int_t
ngx_http_upstream_fair_init_peers(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_server_t *servers;
    ngx_http_upstream_fair_peers_t *peers, *primary;
    ngx_http_upstream_fair_peer_t *peer;
    ngx_uint_t group, count, i, j, n;
    ngx_url_t url;
#if (NGX_HTTP_UPSTREAM_CHECK)
    ngx_uint_t enabled = ngx_http_upstream_check_enabled(us);
#endif

    if (us->servers != NULL) {
        servers = us->servers->elts;
        primary = NULL;
        for (group = 0; group < 2; group++) {
            count = 0;
            for (i = 0; i < us->servers->nelts; i++) {
                if (!!servers[i].backup == group) {
                    if (servers[i].naddrs > (ngx_uint_t) -1 - count) {
                        return NGX_ERROR;
                    }
                    count += servers[i].naddrs;
                }
            }
            if (group && count == 0) {
                break;
            }
            peers = ngx_http_upstream_fair_group(cf, us, count);
            if (peers == NULL) {
                return NGX_ERROR;
            }
            if (primary == NULL) {
                primary = peers;
                us->peer.data = peers;
            } else {
                primary->next = peers;
            }
            n = 0;
            for (i = 0; i < us->servers->nelts; i++) {
                if (!!servers[i].backup != group) {
                    continue;
                }
                for (j = 0; j < servers[i].naddrs; j++) {
                    peer = &peers->peer[n++];
                    peer->sockaddr = servers[i].addrs[j].sockaddr;
                    peer->socklen = servers[i].addrs[j].socklen;
                    peer->name = servers[i].addrs[j].name;
                    peer->weight = servers[i].down ? 0 : servers[i].weight;
                    peer->max_fails = servers[i].max_fails;
                    peer->fail_timeout = servers[i].fail_timeout;
                    peer->down = servers[i].down;
#if (NGX_HTTP_UPSTREAM_CHECK)
                    peer->check_index = (ngx_uint_t) NGX_ERROR;
                    if (enabled && !peer->down) {
                        peer->check_index = ngx_http_upstream_check_add_peer(cf, us, &servers[i].addrs[j]);
                        if (peer->check_index == (ngx_uint_t) NGX_ERROR) {
                            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                               "fair healthcheck registration failed for %V", &peer->name);
                            return NGX_ERROR;
                        }
                    }
#endif
                }
            }
            ngx_sort(peers->peer, count, sizeof(peers->peer[0]), ngx_http_upstream_fair_compare);
        }
        return NGX_OK;
    }

    if (us->port == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "no port in upstream \"%V\"", &us->host);
        return NGX_ERROR;
    }
    ngx_memzero(&url, sizeof(url));
    url.host = us->host;
    url.port = (in_port_t) us->port;
    if (ngx_inet_resolve_host(cf->pool, &url) != NGX_OK) {
        if (url.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%s in upstream \"%V\"", url.err, &us->host);
        }
        return NGX_ERROR;
    }
    peers = ngx_http_upstream_fair_group(cf, us, url.naddrs);
    if (peers == NULL) {
        return NGX_ERROR;
    }
    us->peer.data = peers;
    for (i = 0; i < url.naddrs; i++) {
        peer = &peers->peer[i];
        peer->sockaddr = url.addrs[i].sockaddr;
        peer->socklen = url.addrs[i].socklen;
        peer->name = url.addrs[i].name;
        peer->weight = 1;
        peer->max_fails = 1;
        peer->fail_timeout = 10;
#if (NGX_HTTP_UPSTREAM_CHECK)
        peer->check_index = (ngx_uint_t) NGX_ERROR;
#endif
    }
    return NGX_OK;
}
