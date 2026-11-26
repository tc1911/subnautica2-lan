# SN2LAN — Subnautica 2 直连联机

把 Subnautica 2 的联机数据通道换成 **普通 UDP 直连**（可经 frp 映射到公网），
不再依赖 Epic 的 EOS P2P 中继（relay）。

- 主机：游戏内点「创建多人游戏」，引擎改用 `IpNetDriver` 监听 **UDP 7777**
- 客户端：停在主菜单，按 **F8** 打开游戏内叠加层（画在游戏窗口里），
  地址默认 `主机IP:7777`；点「连接」或按 **F9** 直接连
- 所有玩家都需要安装本 mod，游戏版本必须一致（当前 build 533）

支持的两种模式：

| 模式 | `force_ip_driver` | 数据通道 | 说明 |
| --- | --- | --- | --- |
| IP 直连（默认） | `1` | UDP 7777 | 不需要 EOS 大厅，主机开房、客户端按 IP 进 |
| EOS 隧道 | `0` | UDP 28015 | 保留官方邀请流程，只替换数据面，绕开中继 |

## 工作原理

### IP 直连模式

```
 client A ──UDP 7777──┐
                      ├──> frps（公网）──> frpc ──> host:7777
 client B ──UDP 7777──┘
```

1. 挂钩 `StaticLoadClass`：游戏要加载 `NetDriverEOS` 时换成 `UIpNetDriver`，
   主机因此用普通 UDP 7777 监听（日志 `IpNetDriver listening on port 7777`）。
2. 握手走 **TCP 7777**（与游戏的 UDP 7777 同号，端口空间独立）：
   客机点「连接」后先发 `HELLO`，主机按是否已开房回 `WELCOME`
   （0=可以进、1=还没开房、2=版本不一致），握手成功再调
   `UEngine::SetClientTravel("IP:7777?<登录串>")` 进图。
   IP 模式下不再使用 28015。
3. 登录串优先取配置 `login_options`，否则从游戏日志里抓
   `GetGameLoginOptions returning '...'`（含 `SonarPlayerId`），最后才退回 SteamID。
4. IP 模式下 EOS 语音没有会话，`UUWEVoiceChatWorldSubsystem::Tick` 会空指针崩溃，
   所以 `force_ip_driver=1` 时该 Tick 被跳过（语音因此不可用）。

### EOS 隧道模式（`force_ip_driver = 0`）

挂钩 `EOS_P2P_SendPacket` / `EOS_P2P_ReceivePacket` / `EOS_P2P_GetNextReceivedPacketSize`
（只处理 `socket_name`，默认 `GameNetDriver`），把负载装进 51 字节自定义头
（发送者 PUID、channel、分片号等），经独立 UDP socket 发给主机；主机按包内 PUID
学习 `PUID → 源地址` 并回包。直连被证实可用前仍走原版 EOS 发送，超时自动回退。

## 安装

mod 自带加载器：`dwmapi.dll` 是游戏启动时会加载的系统 DLL，这里换成代理，
在进程内启动 mod。**不需要 UE4SS**（build 533 目前没有可用的 UE4SS：
SN2 fork 的 1.0.0 会 `PS scan timed out`，上游 v4.0.0 进大厅后崩溃）。

1. 把 `dwmapi.dll` 和 `config.ini` 复制到
   `Subnautica2\Binaries\Win64\`（与 `Subnautica2-Win64-Test.exe` 同目录）。
   日志写在同目录的 `sn2lan.log`。
2. Linux/Proton 需要让 Wine 优先加载这份 native DLL：

   ```
   WINEDLLOVERRIDES="dwmapi=n,b" %command%
   ```

   或在 prefix 里写注册表：

   ```
   wine reg add 'HKCU\Software\Wine\DllOverrides' /v dwmapi /d "native,builtin" /f
   ```
3. 卸载：删掉这两个文件即可，游戏本体没有改动。

### 主机

1. 启动游戏，**等主菜单完全出来**（EOS 登录最长约 1 分钟）。
   登录没完成就点创建会报 `Could not retrieve UniqueNetId for Player`。
2. 点「创建多人游戏」并进存档。
3. `sn2lan.log` 出现下面两行即就绪：

   ```
   net driver hook installed: StaticLoadClass -> UIpNetDriver
   IpNetDriver listening on port 7777
   ```
4. 把 **UDP 7777**（游戏数据）和 **TCP 7777**（握手）映射到公网；
   frp 里就是同一个 `remotePort = 7777` 的两条 proxy（`type = "udp"` / `"tcp"`）。

### 客户端

1. `config.ini` 里填好 `host`（打包时用 `HOST_ADDR=你的公网地址` 预填，否则就是 `YOUR-HOST-IP`）。
2. 启动游戏，等主菜单出现。
3. 按 **F8** 打开游戏内面板：地址框默认是 `主机IP:7777`，点「Connect」或按 **F9**。
   主机还没开房也可以点，连不上就再点一次。
4. 叠加层底部会显示握手结果（「主机还没点创建多人游戏」/「连接失败: …」）；
   成功时日志出现：

   ```
   handshake ok, joining <host>:7777
   manual join: ClientTravel to <host>:7777?...
   ```

如果想让客户端自动进图，在 `config.ini` 里取消 `auto_connect` 的注释即可。

## 身份：SteamID、SonarPlayerId 和游客

登录完成后游戏会生成登录串：

```
?SonarPlayerId=1234567?PlatformUserId=7656119XXXXXXXXXX?PlatformProvider=STEAM
```

- `PlatformUserId` 是 SteamID64，服务器靠它匹配玩家存档（物品、角色）。
- `SonarPlayerId` 由 `api.live.subnautica.net` 登录接口下发，需要能访问该域名。
- 接口不通时 `SonarPlayerId` 为空，但只要有 SteamID，主机存档仍能按 Steam 账号
  匹配到原来的 PlayerState；只有连 SteamID 都没有时才会变成「游客」，人物创建变灰。

客户端会先等 45 秒登录串，再退回 SteamID 连接；期间日志每 15 秒提示一次。
手动指定可以用 `config.ini` 的 `login_options`（原样拼在 `IP:7777` 后面）。

## 游戏内叠加层（F8 / F9）

`dwmapi.dll` 用游戏自带的 ImGui 在主菜单/游戏里画一个面板，不需要切换窗口：

- Role：主机或客户端、目标地址、握手/游戏端口
- Status：主机是否已开房、探测结果、隧道 peer 数
- Identity：是否已读到账号登录串（`SonarPlayerId`）
- Host address / Port：**直接点击输入框即可编辑**（键盘输入，`Backspace`
  删除，`Enter` 或 `Esc` 结束编辑），`Connect` 按当前地址与端口发起握手
- **F8** 显示/隐藏，**F9** 用当前地址直接连接（不依赖鼠标，适合点击不方便时）
- 纯键盘操作：**Tab / ↑↓** 在「地址 → 端口 → Connect → Hide」之间切换，**Enter** 开始编辑（再按 Enter 结束）或执行连接，**Esc** 收起面板
- 鼠标点击：面板内点击会被 mod 截获注入 ImGui；若点击位置对不上（常见于 Linux + niri 这类会平移窗口的合成器），按 **F7** 后点一次蓝色「+」重新校准；Windows 上一般不需要
- `Hide`：收起面板
- 面板打开时，落在面板上的鼠标点击会被 mod 吃掉，不会误触游戏菜单
- **位置校准**：第一次运行会显示 `SN2LAN setup`，点一下蓝色 `+` 即可把面板
  和鼠标对齐（偏移存到 `Binaries\Win64\sn2lan_ui.txt`）；以后面板与鼠标
  错位（换分辨率、窗口被拖动）按 **F7** 重新校准

面板用 ASCII 绘制（游戏自带 ImGui 字体没有中文字形）；桌面小窗版本仍保留，
在 `config.ini` 里设 `overlay_mode = window` 可回退。若游戏 60 秒内没有产生
ImGui 帧（例如测试 harness），会自动回退到桌面小窗。

## 配置

完整键见 `config.example.ini`，常用的：

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `enabled` | `true` | 总开关 |
| `role` | `host` | `host` / `client` |
| `force_ip_driver` | `1` | `1` 用 UIpNetDriver 直连；`0` 走 EOS 隧道 |
| `bind_port` | `28015` | EOS 隧道本地端口；客户端建议 `0`（系统分配） |
| `control_port` | `7777` | IP 模式 TCP 握手端口（与游戏 UDP 同号） |
| `host` / `host_port` | — | 客户端连接的主机地址（隧道用） |
| `overlay` | `true` | 游戏内叠加层（F8/F9 手动加入） |
| `overlay_mode` | `imgui` | `imgui` 画在游戏窗口内；`window` 用独立小窗（兼容性回退） |
| `auto_connect` | 空 | 可选：客户端自动进图地址，如 `1.2.3.4:7777` |
| `auto_connect_delay` | `20` | 主机开房后等待多少秒再进（1–600） |
| `login_options` | 空 | 手动指定登录串，空则自动抓日志 |
| `mtu` | `1400` | 隧道分片大小（512–2000） |
| `debug` | `true` | 详细日志 |

## frp

`deploy/frps.toml` 与 `deploy/frpc.toml` 是模板，需要映射：

- **TCP 7777 + UDP 7777**（IP 模式必需，同一个 remotePort）
- **UDP 28015**（仅 EOS 隧道模式）

`frpc.toml` 里的 `serverAddr` / `token` 改成自己的服务器。

## 已知限制

- EOS 语音聊天在 IP 模式下被禁用（第三方语音替代）。
- 依赖 Sonar 账号的功能（云端同步等）在 `SonarPlayerId` 为空时不可用。
- 只支持 IPv4；主机存档制，没有专用服务器。
- 主机和客户端必须使用同一游戏版本；主机崩溃/退出后客户端会断开。
- 仅供与同意使用 mod 的玩家联机，注意游戏 ToS 风险。

## 排障

日志：`Binaries\Win64\sn2lan.log`；游戏日志：
`Subnautica2\Saved\Logs\Subnautica2.log`。

| 现象 | 处理 |
| --- | --- |
| `EOS SDK module not found` | 确认 `Engine\Binaries\Win64\EOSSDK-Win64-Shipping.dll` 存在 |
| `MH_CreateHook ... NOT_EXECUTABLE` | mod 与游戏版本不匹配（当前仅 build 533） |
| 主机点创建报 `Could not retrieve UniqueNetId` | EOS 登录未完成，重启 Steam/游戏后再试 |
| 客户端一直不进 | 看叠加层提示；连不上多半是 TCP/UDP 7777 未放通 |
| 进图瞬间崩溃 | 确认 `force_ip_driver=1` 且日志有 `voice chat tick suppressed` |
| 人物创建是灰的 | 登录串里连 SteamID 都没有，检查 `login_options` |

## 构建与测试

```
scripts/build.sh      # MinGW 交叉编译 sn2lan.dll + dwmapi.dll
scripts/test.sh       # 协议单测
scripts/test-tunnel-wine.sh   # Wine 下 UDP 隧道集成测试
scripts/test-integration-wine.sh  # 假 EOS DLL + 端到端
scripts/package.sh    # 生成 host/client 两个 zip（HOST_ADDR 可覆盖）
```

线协议见 [`docs/protocol.md`](docs/protocol.md)，测试说明见
[`docs/testing.md`](docs/testing.md)。

## 许可

MIT；内嵌 MinHook（BSD 2-Clause），见 `LICENSE`。
