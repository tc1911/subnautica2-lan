# 测试与验证

## 自动测试（Linux）

需要 `x86_64-w64-mingw32-g++` 与 `wine`。

| 命令 | 覆盖 |
| --- | --- |
| `./scripts/test.sh` | 协议单测：分片/重组、坏帧、超时丢弃、在途消息上限、接收队列上限 |
| `./scripts/test-tunnel-wine.sh` | Wine 下真实 UDP 回环 + 分片重组（18 项） |
| `./scripts/test-integration-wine.sh` | 端到端：假 `EOSSDK-Win64-Shipping.dll` + 两个进程 + 真实 hook |
| `./scripts/build.sh` | 交叉编译 `sn2lan.dll` 与 `dwmapi.dll` |
| `./scripts/package.sh` | 生成 `dist/SN2LAN-{host,client}-<version>.zip` |

`test-integration-wine.sh` 在 `build/integration/` 放一个假的
`EOSSDK-Win64-Shipping.dll`（P2P 函数永远返回「没有包」），两个 `harness.exe`
分别以 host/client 角色通过 `sn2lan.dll` 的导出函数收发。通过标准：

- host 学到 client 的 PUID 与源地址，收到 4000 字节（跨 MTU 分片）的包；
- client 在收到 host 回包后转入直连（日志 `direct tunnel to host is alive`）；
- 引导阶段 client 的包仍走 EOS（`eos_sends > 0`），之后
  `direct_sends` 增长、`parse_errors=0`。

这套测试只覆盖 **EOS 隧道**（`force_ip_driver=0`）；IP 直连模式依赖引擎的
`UIpNetDriver`，只能在真机游戏里验证。

## 真机测试（Windows，IP 直连模式）

两端都必须用同一个包版本（启动日志第一行 `SN2LAN 0.1.0 (build ...)` 要一致）。

### 主机

1. 把 `dwmapi.dll`、`config.ini` 放进 `Subnautica2\Binaries\Win64\`
   （不需要 UE4SS；当前所有 UE4SS 都不支持 build 533）。
2. 启动 Steam 并登录，再启动游戏，**等主菜单完全出来**（EOS 登录最长约 1 分钟）。
3. 点「创建多人游戏」并进存档。以下四行都应出现：

   ```
   SN2LAN 0.1.0 (build ...) loading from ...\Binaries\Win64
   net driver hook installed: StaticLoadClass -> UIpNetDriver
   control server listening on TCP 7777
   IpNetDriver listening on port 7777
   ```

4. 公网联机时把 **TCP 7777 + UDP 7777** 都映射出去（frp 用同一 `remotePort`）。

### 客户端

1. 同样放入 `dwmapi.dll`、`config.ini`；`host` 填主机地址（或打包时用
   `HOST_ADDR=` 预填）。
2. 启动游戏，停在主菜单，按 **F8** 打开面板：
   - 面板第二行显示握手结果；地址框里是 `主机IP:7777`；
   - 点 `Connect`，或直接按 **F9**（不依赖鼠标）。
3. 成功时 `sn2lan.log`：

   ```
   handshake ok, joining <host>:7777
   manual join: ClientTravel to <host>:7777?PlatformUserId=...
   net driver hook installed: StaticLoadClass -> UIpNetDriver
   ```

   游戏日志 `Subnautica2\Saved\Logs\Subnautica2.log` 出现
   `Welcomed by server (Level: /Game/Maps/Main/L_Main)` 即已进入主机世界。

### 鼠标 / 键盘

- 键盘全程可用：F8 显示隐藏，Tab / ↑↓ 选行，Enter 编辑或连接，Esc 收起，F9 直连。
- 鼠标点击由 mod 截获后写给游戏的 ImGui。**第一次运行**会要求校准：对着蓝色 `+`
  点两下（面板会在两次点击之间移动），两次结果不一致会自动重来。
- 之后若换了分辨率、拖动过窗口或点击明显偏移，按 **F7** 重新点两下即可；
  实在不行删掉 `Binaries\Win64\sn2lan_ui.txt` 再按 F7。

## 排查顺序

| 现象 | 先看 |
| --- | --- |
| 日志里没有 `SN2LAN ... loading from` | `dwmapi.dll` 没放到 `Binaries\Win64\`，或被别的 `dwmapi.dll` 抢先加载 |
| `MH_CreateHook ... NOT_EXECUTABLE` | mod 与游戏版本不匹配（当前仅 build 533） |
| `handshake ... cannot resolve host` | `config.ini` 里还是 `YOUR-HOST-IP` 占位符，或域名写错 |
| `handshake ... no reply` | 主机侧没有 `control server listening on TCP 7777`（旧包/没启动），或 TCP 7777 没映射 |
| `handshake ... connect timeout` | TCP 7777 不通：frp 只映射了 UDP、防火墙、云安全组 |
| 握手成功但进不去 | 主机侧看 `IpNetDriver listening on port 7777`；UDP 7777 是否映射；进图瞬间是否崩溃（语音 Tick） |
| 面板能显示但点不动 | 校准问题，按 F7 点两下 `+`；或先用键盘 Tab/Enter/F9 |
| 某位玩家掉帧、卡顿 | 与网络无关：先单人模式对照，检查是否首次着色器（PSO）编译、GC 或机器性能 |

## 回归 EOS 隧道模式（可选）

把两端 `force_ip_driver` 改成 `0`，走官方邀请流程：

- 主机：`role = host`、`bind_port = 28015`（或任意）；
- 客户端：`role = client`、`host` = 主机公网地址、`host_port` = 映射端口；
- 日志：`EOS P2P hooks installed (socket filter: GameNetDriver)`；主机
  `peer <PUID> learned at <ip:port>`；客户端 `direct tunnel to host is alive`；
- 60 秒一条 `health:`，`direct_sends` / `direct_receives` 持续增长、
  `parse_errors` 保持 0；若只有 `eos_fallback_sends` 增长，说明直连没建立，
  检查 frp UDP 映射、防火墙、`bind_address`（应为 `0.0.0.0`）。
