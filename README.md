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
- Splits peer construction into `peers/` and shared statistics into `shm/`.
- Gives each configuration cycle its own shared-memory zone and allocates all group statistics while loading the configuration.
- Follows native Nginx semantics for passive failures, `max_conns`, and backup selection.
- Keeps fair state in the `upstream_fair` zone when an upstream also declares `zone`, so fair can coexist with native `zone`, `keepalive`, and active checks.
- Recovers per-worker in-flight counts after a worker is respawned, and releases a group lock on Unix when its holder process no longer exists.
- Keeps required counters and routing rules active without `--with-debug`.

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
- `server ... max_conns=N;` limits that address by the cross-worker in-flight count. The default `0` means unlimited. Connection attempts are included, while idle connections cached by `keepalive` are not. The parameter works whether it appears before or after `fair`. Both this limit and the peak weight limit apply together, including to a true single peer.
- `upstream_fair_shm_size size;` is configured in `http`. The default is 1MiB, the minimum is eight system pages, and the value is page-aligned. Changing or removing the directive takes effect on reload. The size is fixed. If it is too small, startup fails or reload is rejected while the previous configuration remains active. The error log reports the current size and the statistics allocation size, excluding slab overhead, and asks for a larger value. Each group stores scheduler state plus `peers × workers` in-flight counters.

## Primary/Backup Groups and Active Health Checks

```nginx
upstream backend {
    fair;
    zone backend 1M;
    server 127.0.0.1:5000 weight=2 max_fails=2 fail_timeout=10s;
    server 127.0.0.1:5001;
    server 127.0.0.1:5002 backup;

    keepalive 64;
    check type=tcp interval=3000 timeout=1000 rise=2 fall=5;
}
```

The `check` directive is provided by [nginx-healthcheck-module](https://github.com/Akvicor/nginx-healthcheck-module) through a joint static build. All non-static-down addresses in the primary and backup groups of an explicit upstream are registered for checks. Sorted check indexes still correspond to their addresses; if checks are configured but registration fails, the configuration load is rejected.

Each new request starts in the primary group and enters the backup group only when the primary group has no eligible candidate. In normal mode, busy primary peers still participate in primary-group scoring. A request that has entered the backup group continues retrying within that group; after a primary peer becomes eligible again, new requests prefer the primary group. Backup peers do not share traffic while any primary peer can accept a request. When every primary peer has reached an explicit capacity limit (`max_conns` or peak weight), fair follows native backup selection. Native request lifecycle handling continues to enforce limits such as `proxy_next_upstream`.

At least one primary peer must be configured. A primary peer may be statically down, but a configuration containing only backup peers fails to load.

Static down, active down, passive failure cooldown, capacity limits, and request tried state each participate in eligibility checks. Passive failures follow native `max_fails` and `fail_timeout` behavior: after the threshold is reached, the peer cools down, and the next window admits one business attempt. Success clears `fails`; failure starts another cooldown. Other requests avoid that address during the window. A long-running attempt that crosses the next window follows the native retry rule. `max_fails=0` keeps failure history out of eligibility and scoring, and a peer below the failure threshold remains eligible for idle selection.

Active recovery to up updates only active health; traffic still has to satisfy passive cooldown. When both primary and backup candidates are exhausted, Nginx returns 502 and passive fail counts are preserved. A true single peer still ignores passive cooldown, while static and active down state continue to filter it. A fair-only build provides the same primary/backup and busy-aware scheduling behavior; omit the healthcheck module's `check` directive in that configuration.

An upstream `zone` can be configured together with `fair`, `keepalive`, and `check`. Fair keeps its scheduler state in the separate `upstream_fair` zone sized by `upstream_fair_shm_size`. The declared native zone is still created and initialized by Nginx, but fair does not use its memory. Other native upstreams sharing that zone continue to use it. Place `fair` before `keepalive` so the native keepalive module wraps fair's get and free callbacks.

## Counters and Lifecycle

Selecting and returning a peer increments or decrements the peer's `nreq` and its group's `total_nreq`, including the connection attempt phase. Repeated free operations are idempotent. Bitmap state, failure reports, SSL sessions, and statistics always belong to the actually selected group and peer.

Each configuration cycle owns an independent shared-memory zone. Primary and backup statistics are allocated and linked while the configuration loads. Draining workers keep their old mapping, and statistics start at zero after reload. Shared statistics record each worker's in-flight contribution. A respawned worker reclaims its predecessor's residue and rebuilds the aggregate counters. On Unix, startup recovery releases a group lock when the holder PID no longer exists; a live or stopped holder keeps the lock.

Required counters and all routing rules run with or without `--with-debug`; runtime log levels only control diagnostics. `--with-debug`, compiler optimization level, and `-g` debug symbols are independent settings. A respawned worker uses the statistics of the effective configuration. A failed load keeps the previous configuration, and requests accepted before a successful reload drain according to native Nginx rules.

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
