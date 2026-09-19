# ngx_http_upstream_fair_module

<p align="center">
  <strong>English</strong> | <a href="README.zh-CN.md">简体中文</a>
</p>

[Blog](https://www.ksyaki.com/archives/nginx-fu-zai-jun-heng-mo-kuai)

A busy-aware upstream load balancer for Nginx: backends handling more in-flight requests receive fewer new requests, while idle backends receive more. This tree is based on Debian `libnginx-mod-http-upstream-fair` (upstream `0.0~git20120408.a18b409`) and includes Debian patches for dynamic module builds, OpenSSL 1.1+, and Nginx 1.11.6+.

## Differences from Upstream

Compared with the original `a18b409` tree, this maintained version:

- Applies Debian compatibility patches for dynamic module builds, OpenSSL 1.1+, and Nginx 1.11.6+.
- Adds optional integration with [nginx-healthcheck-module](https://github.com/Akvicor/nginx-healthcheck-module) for active peer health filtering.
- Splits peer and group state into the `peers` module and hardens primary/backup group selection.
- Fixes request accounting and free lifecycle handling so counters, tried bitmaps, failure reports, and SSL sessions belong to the actually selected group.
- Keeps required counters and routing rules active without `--with-debug`, and publishes runtime state through Nginx's native configuration commit callback.

## Build

```bash
git clone https://github.com/nginx/nginx.git
git clone https://github.com/Akvicor/nginx-upstream-fair.git

cd nginx
git checkout release-1.26.3
./auto/configure --add-module=../nginx-upstream-fair
make
make install
```

Use `--add-dynamic-module=../nginx-upstream-fair` instead when a dynamic module build is needed.

## Usage

```nginx
upstream backend {
    fair;
    server 127.0.0.1:5000;
    server 127.0.0.1:5001;
    server 127.0.0.1:5002;
}
```

## Directives and Scheduling

```nginx
fair [no_rr] [weight_mode=idle|weight_mode=peak];
```

- The default mode selects a backend using in-flight requests, assignment history, and weights. Idle candidates are preferred; other eligible candidates participate in busy scoring.
- `no_rr` controls the existing round-robin cursor policy and can be combined with either weight mode.
- `weight_mode=idle` uses weight as the in-flight reference bound during idle selection; a peer can still enter busy selection afterwards. With `no_rr`, the existing preference for lightly loaded peers is preserved.
- `weight_mode=peak` treats weight as the concurrent request capacity when multiple peers are available. When a backup group exists, a full primary group can overflow into the backup group. The existing single-peer special case is preserved when there is exactly one peer and no backup group.
- `upstream_fair_shm_size size;` is configured in `http`. Its default and minimum value is eight system pages, and the configured value is page-aligned. Restart Nginx to resize an existing shared memory zone.

## Primary/Backup Groups and Active Health Checks

```nginx
upstream backend {
    fair;
    server 127.0.0.1:5000 weight=2 max_fails=2 fail_timeout=10s;
    server 127.0.0.1:5001;
    server 127.0.0.1:5002 backup;

    check type=tcp interval=3000 timeout=1000 rise=2 fall=5;
}
```

The `check` directive is provided by [nginx-healthcheck-module](https://github.com/Akvicor/nginx-healthcheck-module) through a joint static build. All non-static-down addresses in the primary and backup groups of an explicit upstream are registered for checks. Sorted check indexes still correspond to their addresses; if checks are configured but registration fails, the configuration load is rejected.

Each new request starts in the primary group and enters the backup group only when the primary group has no eligible candidate. In normal mode, busy primary peers still participate in primary-group scoring. A request that has entered the backup group continues retrying within that group; after a primary peer becomes eligible again, new requests prefer the primary group. Native request lifecycle handling continues to enforce limits such as `proxy_next_upstream`.

At least one primary peer must be configured. A primary peer may be statically down, but a configuration containing only backup peers fails to load.

Static down, active down, passive failure cooldown, and request tried state each participate in eligibility checks. Active recovery to up updates only active health; traffic still has to satisfy passive cooldown. When both primary and backup candidates are exhausted, passive fail counts are preserved. The legacy no-backup exhaustion path and true single-peer passive special case remain unchanged; a true single peer is still filtered by static and active down state. A fair-only build provides the same primary/backup and busy-aware scheduling behavior; omit the healthcheck module's `check` directive in that configuration.

## Counters and Lifecycle

Selecting and returning a peer increments or decrements the peer's `nreq` and its group's `total_nreq`, including the connection attempt phase. Repeated free operations are idempotent. Bitmap state, failure reports, SSL sessions, and statistics always belong to the actually selected group and peer. Old generation statistic blocks are checked through the existing traversal entry and retained while their in-use count is non-zero.

Required counters and all routing rules run with or without `--with-debug`; runtime log levels only control diagnostics. `--with-debug`, compiler optimization level, and `-g` debug symbols are independent settings.

The shared-memory runtime entry is published through Nginx's native configuration commit callback. If loading fails, the old entry remains active, and replacement workers continue to use the valid configuration. After a successful reload, old requests drain according to native Nginx rules.

## Joint Build with Health Checks

Active health-check integration requires a joint static build:

```bash
git clone https://github.com/nginx/nginx.git
git clone https://github.com/Akvicor/nginx-healthcheck-module.git
git clone https://github.com/Akvicor/nginx-upstream-fair.git

cd nginx
git checkout release-1.26.3
git apply ../nginx-healthcheck-module/nginx_healthcheck_for_nginx_1.26+.patch
./auto/configure \
    --with-stream \
    --add-module=../nginx-healthcheck-module \
    --add-module=../nginx-upstream-fair
make
```

`NGX_HTTP_UPSTREAM_CHECK` isolates healthcheck-only headers, fields, and symbols from fair-only builds.

## Verification

After building, run `nginx -t` and verify the configuration with real upstream traffic.
