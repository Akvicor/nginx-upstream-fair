# ngx_http_upstream_fair_module

<p align="center">
  <a href="README.md">English</a> | <strong>简体中文</strong>
</p>

[博客链接](https://www.ksyaki.com/archives/nginx-fu-zai-jun-heng-mo-kuai)

按后端当前忙闲分配请求：正在处理请求较多的后端少分，空闲后端多分。来源为 Debian `libnginx-mod-http-upstream-fair`（上游 `0.0~git20120408.a18b409`），并已应用 Debian 补丁以支持动态模块、OpenSSL 1.1+ 和 nginx 1.11.6+。

## 与上游版本的差异

相比上游 `a18b409` 版本，当前维护版本：

- 应用 Debian 兼容补丁，支持动态模块、OpenSSL 1.1+ 和 nginx 1.11.6+。
- 增加与 [nginx-healthcheck-module](https://github.com/Akvicor/nginx-healthcheck-module) 的可选联动，按主动健康检查结果过滤 peer。
- 将 peer 构造拆到 `peers/`，共享统计拆到 `shm/`。
- 每个配置周期使用独立共享区，并在加载配置时分配全部组统计。
- 被动失败、`max_conns` 和 backup 选择遵循 Nginx 原生语义。
- upstream 同时声明 `zone` 时，fair 的状态仍放在 `upstream_fair` 共享区，因此可以与原生 `zone`、`keepalive` 和主动检查共存。
- worker 重生后回收该 worker 的在途计数；Unix 上持锁进程已不存在时，启动恢复路径释放组锁。
- 必要计数和选路规则不依赖 `--with-debug`。

## 构建

```bash
git clone https://github.com/nginx/nginx.git
git clone https://github.com/Akvicor/nginx-upstream-fair.git

cd nginx
git checkout release-1.26.3
./auto/configure --add-module=../nginx-upstream-fair
make
make install
```

需要动态模块时，将 `--add-module` 替换为 `--add-dynamic-module`。

## 使用

```nginx
upstream backend {
    fair;
    server 127.0.0.1:5000;
    server 127.0.0.1:5001;
    server 127.0.0.1:5002;
}
```

## 指令与调度

```nginx
fair [no_rr] [weight_mode=idle|weight_mode=peak];
```

- 默认模式结合当前在用请求、请求分配历史和权重选择后端；优先空闲候选，其余合格候选参与 busy 评分。
- `no_rr` 控制既有的轮转游标策略，可与两种 weight_mode 配合。
- `weight_mode=idle` 在 idle 选择时将 weight 作为在用请求参考界限，随后仍可进入 busy 选择；配合 no_rr 时沿用对已轻载节点的既有偏好。
- `weight_mode=peak` 将 weight 作为多节点选择的并发容量上限；有备组时，容量已满的主组可以进入备组。真正单节点且无备组保留既有单节点特例。
- `server ... max_conns=N;` 按跨 worker 在用请求数限制该地址，默认 `0` 表示无限制。计数包含连接尝试，不包含 keepalive 缓存的空闲连接；参数写在 `fair` 前后均生效。它与 peak 的 weight 上限同时生效，真单节点同样遵守。
- `upstream_fair_shm_size size;` 位于 `http`，默认 1MiB，最小 8 个系统页，按页对齐。修改或删除指令后 reload 生效。大小固定；容量不足时启动失败，或拒绝 reload 并保留旧配置。错误日志给出当前大小和统计分配规模（不含 slab 管理开销），并提示增大该值。每组存放调度状态以及 `后端数 × worker 数` 个在用计数。

## 主备与主动健康检查

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

`check` 来自 [nginx-healthcheck-module](https://github.com/Akvicor/nginx-healthcheck-module)，通过共同静态构建提供。显式 upstream 内主、备两组所有非静态 down 地址各自注册检查，排序后检查索引仍与地址对应；配置了检查却注册失败会拒绝本次加载。

每个新请求从主组开始，主组没有合格候选时才进入备组。普通模式下主节点忙碌仍参与主组评分；已有请求进入备组后在该组继续重试，新请求在主节点恢复合格后重新优先主组。主组还有可选节点时，backup 节点不分摊流量；主节点全部达到显式容量限制（`max_conns` 或 peak 的 weight）时，按原生规则进入 backup 节点。`proxy_next_upstream` 的次数等限制继续由原生请求生命周期执行。

配置至少保留一个主节点，主节点可以静态 down；只有 backup 节点的配置会加载失败。

静态 down、主动 down、被动失败冷却、容量限制和请求 tried 分别参与资格判断。被动失败按原生 `max_fails`/`fail_timeout` 处理：达到阈值后进入冷却，冷却期满先放行一个业务尝试；成功则清零 fails，失败则进入下一轮冷却。同一窗口内的其他请求继续避开该地址。跨越下一个窗口的长请求遵循原生重试规则。`max_fails=0` 时失败历史不影响资格和评分，未达失败阈值的节点仍参与空闲选择。

主动恢复 up 只更新主动健康，业务仍须满足被动冷却。主备全部耗尽时返回 502 并保留 fails。真单节点仍忽略被动冷却，同时继续过滤静态和主动 down。fair-only 构建提供相同的主备与忙闲调度，配置示例省略 healthcheck 的 `check` 即可。

upstream 中的 `zone` 可以与 `fair`、`keepalive`、`check` 一起配置。fair 的调度状态位于独立的 `upstream_fair` 共享区，容量由 `upstream_fair_shm_size` 设置。声明的原生 zone 仍由 Nginx 创建和初始化，fair 不使用它的内存；共用同名 zone 的其他原生 upstream 照常运行。`fair` 应写在 `keepalive` 前面，使原生 keepalive 包装 fair 的 get/free 回调。

## 计数与生命周期

实际选中及归还分别增减 peer 的 `nreq` 和所属组的 `total_nreq`，包含连接尝试阶段；重复 free 幂等。位图、失败回报、SSL session 和统计始终属于实际选中的组与 peer。

每个配置周期拥有独立共享区，加载配置时分配并关联主备全部统计块。旧 worker 排空期间继续使用旧映射，reload 后统计从零开始。共享统计记录每个 worker 的在用贡献。worker 重生后回收前任残留并重建汇总计数。Unix 启动恢复路径在确认组锁持有者 PID 已不存在时释放残留锁；存活或暂停的持有者继续保留锁。

必要计数和所有选路规则在带/不带 `--with-debug` 时都执行，运行日志级别只控制诊断。`--with-debug` 与编译器的优化级别、`-g` 调试符号是独立设置。重生 worker 使用有效配置的统计；加载失败保留旧配置，成功 reload 的旧请求按原生规则排空。

## 与 Healthcheck 共同构建

主动健康检查联动需要共同静态构建：

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

`NGX_HTTP_UPSTREAM_CHECK` 隔离 fair-only 的头文件、字段和符号依赖。

## 验证

构建完成后使用 `nginx -t` 和实际 upstream 流量验证配置。
