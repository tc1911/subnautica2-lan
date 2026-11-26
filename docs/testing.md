# 测试与验证

## 本地（Linux）

需要 `x86_64-w64-mingw32-g++` 与 `wine`。

```bash
./scripts/test.sh                  # 协议单测（分片、坏帧、超时、内存上限）
./scripts/test-tunnel-wine.sh      # Wine 下真实 UDP 回环 + 分片重组
./scripts/test-integration-wine.sh # 端到端：假 EOSSDK + 两个进程 + 真实 hook
./scripts/build.sh                 # 交叉编译 DLL
./scripts/package.sh               # 生成 dist/SN2LAN 与 zip
```

`test-integration-wine.sh` 会在 `build/integration/` 放一个假的
`EOSSDK-Win64-Shipping.dll`（P2P 函数永远返回“没有包”），两个 `harness.exe`
分别以 host/client 角色通过 `sn2lan.dll` 的导出函数收发。通过标准：

- host 学到 client 的 PUID 与源地址，收到 4000 字节（跨 MTU 分片）的包；
- client 在收到 host 回包后转入直连（日志 `direct tunnel to host is alive`）；
- 引导阶段 client 的包仍走 EOS（`eos_sends > 0`），之后
  `direct_sends` 增长、`parse_errors=0`。

## 真机（Windows）

1. 把 `dwmapi.dll`、`config.ini` 放进 `Binaries\Win64\`（不需要 UE4SS；
   现在的 UE4SS 都不支持 build 533）。
2. 启动一次游戏（原步骤：把 `SN2LAN` 放进 `ue4ss\Mods\` 并在 `Mods\mods.txt` 加
   `SN2LAN : 1`（或放一个空的 `enabled.txt`）。
3. 把 `config.example.ini` 复制为 `config.ini`：
   - 主机：`role = host`，`bind_port` = 想开放的 UDP 端口；
   - 客户端：`role = client`，`host` = 主机公网地址，`host_port` = 映射后的端口。
4. 启动游戏，进入同一局（官方邀请/大厅流程）。
5. 检查双方 `Binaries\Win64\sn2lan.log`：
   - `EOS P2P hooks installed (socket filter: GameNetDriver)`；
   - 主机：`peer <client PUID> learned at <ip:port>`；
   - 客户端：`direct tunnel to host is alive`；
   - 60 秒一条 `health:`，`direct_sends`/`direct_receives` 持续增长、
     `parse_errors` 保持 0。
6. 若只有 `eos_fallback_sends` 增长，说明直连没建立：检查 frp UDP 映射、
   防火墙、`bind_address`（应为 `0.0.0.0`）、以及主机是否真的收到了包。

## 排查顺序

| 现象 | 先看 |
|------|------|
| 日志里没有 `loaded EOS SDK` | dwmapi 代理是否加载；EOSSDK 路径是否被裁剪 |
| 只有 fallback，直连不成立 | frp/防火墙/UDP 端口；`peer ... learned` 是否出现 |
| `parse_errors` 增长 | mtu 是否大于链路 UDP 上限；是否有旧版本客户端 |
| 进游戏后立刻掉线 | 两端 DLL/config 版本是否一致；`socket_name` 是否为 `GameNetDriver` |
