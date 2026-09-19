# ngx_http_upstream_fair_module

按后端当前忙闲分配请求：正在处理请求较多的后端少分，空闲后端多分。来源为 Debian `libnginx-mod-http-upstream-fair`（上游 `0.0~git20120408.a18b409`），并已应用 Debian 补丁以支持动态模块、OpenSSL 1.1+ 和 nginx 1.11.6+。

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

- 默认模式结合当前在用请求、请求分配历史和权重选择后端；优先空闲候选，
  其余合格候选参与 busy 评分。
- `no_rr` 控制既有的轮转游标策略，可与两种 weight_mode 配合。
- `weight_mode=idle` 在 idle 选择时将 weight 作为在用请求参考界限，随后仍可
  进入 busy 选择；配合 no_rr 时沿用对已轻载节点的既有偏好。
- `weight_mode=peak` 将 weight 作为多节点选择的并发容量上限；有备组时，
  容量已满的主组可以进入备组。真正单节点且无备组保留既有单节点特例。
- `upstream_fair_shm_size size;` 位于 `http`，默认和最小值为 8 个系统页，
  按页对齐；修改已有共享区大小需要重启。

## 主备与主动健康检查

```nginx
upstream backend {
    fair;
    server 127.0.0.1:5000 weight=2 max_fails=2 fail_timeout=10s;
    server 127.0.0.1:5001;
    server 127.0.0.1:5002 backup;

    check type=tcp interval=3000 timeout=1000 rise=2 fall=5;
}
```

`check` 来自 [nginx-healthcheck-module](https://github.com/Akvicor/nginx-healthcheck-module)，
通过共同静态构建提供。显式 upstream 内主、备两组所有非静态 down 地址各自注册检查，
排序后检查索引仍与地址对应；配置了检查却注册失败会拒绝本次加载。

每个新请求从主组开始，主组没有合格候选时才进入备组。普通模式下主节点忙碌仍参与
主组评分；已有请求进入备组后在该组继续重试，新请求在主节点恢复合格后重新优先主组。
`proxy_next_upstream` 的次数等限制继续由原生请求生命周期执行。
配置至少保留一个主节点，主节点可以静态 down；只有 backup 节点的配置会加载失败。

静态 down、主动 down、被动失败冷却和请求 tried 分别参与资格判断。主动恢复 up
只更新主动健康，业务仍须满足被动冷却；主备全部耗尽保留被动 fails。
无备组的旧耗尽处理、真单节点的被动特例继续沿用；真单节点同样过滤静态和主动 down。
fair-only 构建提供相同的主备与忙闲调度，配置示例省略来自 healthcheck 的 `check` 即可。

## 计数与生命周期

实际选中及归还分别增减 peer 的 `nreq` 和所属组的 `total_nreq`，包含连接尝试阶段；
重复 free 幂等。位图、失败回报、SSL session 和统计始终属于实际选中的组与 peer。
旧 generation 统计块在原有遍历入口检查在用数量，非零时保留。

必要计数和所有选路规则在带/不带 `--with-debug` 时都执行，运行日志级别只控制诊断。
`--with-debug` 与编译器的优化级别、`-g` 调试符号是独立设置。
共享区运行入口在 Nginx 原生配置提交回调中发布；加载失败保持旧入口，重生 worker
继续使用有效配置，成功 reload 的旧请求按原生规则排空。

## 构建与验证

`config` 保留既有静态/动态接入。主动检查联合支持以与
[nginx-healthcheck-module](https://github.com/Akvicor/nginx-healthcheck-module) 的共同静态构建为准；
`NGX_HTTP_UPSTREAM_CHECK` 隔离 fair-only 的头文件、字段和符号依赖。
构建完成后使用 `nginx -t` 和实际 upstream 流量验证配置。
