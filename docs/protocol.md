# SN2LAN 线协议

所有整数小端序。mod 有两种数据路径，协议互不相干：

| 模式 | 端口 | 谁在处理 | 本文档 |
| --- | --- | --- | --- |
| IP 直连（`force_ip_driver = 1`，默认） | TCP 7777 + UDP 7777 | 握手是 mod 的；游戏数据是引擎的 `IpNetDriver`，mod 不参与 | 第 1 节 |
| EOS 隧道（`force_ip_driver = 0`） | UDP 28015 | 全部是 mod 的（51 字节头 + 分片） | 第 2 节 |

---

## 1. IP 直连：控制通道（TCP 7777）

游戏数据走引擎自己的 `UIpNetDriver`（普通 UDP 7777，包格式是 UE 的，mod 不解析）。
mod 只在**同一端口号的 TCP** 上提供一次握手，回答「现在能不能进」。

### 报文格式

包头 12 字节：

| 偏移 | 大小 | 字段 | 说明 |
|-----:|-----:|------|------|
| 0 | 4 | magic | `0x43324E53`（ASCII `SN2C`） |
| 4 | 2 | version | 包头版本，当前 1 |
| 6 | 2 | type | 1 = HELLO，2 = WELCOME |
| 8 | 4 | length | 负载长度（≤ 64） |

HELLO 负载 4 字节：

| 偏移 | 大小 | 字段 | 说明 |
|-----:|-----:|------|------|
| 0 | 2 | protocol | 联机协议版本，当前 1 |
| 2 | 2 | reserved | 0 |

WELCOME 负载 4 字节：

| 偏移 | 大小 | 字段 | 说明 |
|-----:|-----:|------|------|
| 0 | 1 | status | 0 = 可以进，1 = 主机还没开房，2 = 版本不一致 |
| 1 | 3 | reserved | 0 |

### 流程

1. 客户端 `connect()` 到 `主机地址:7777`（超时 5 秒），发 HELLO。
2. 主机校验 magic / 包版本 / 协议版本，回一个 WELCOME 后立即关闭连接。
   - `protocol` 不匹配 → `status = 2`
   - `protocol` 匹配但还没监听游戏端口 → `status = 1`
   - 否则 → `status = 0`
3. 客户端读到 `status = 0` 后调用 `UEngine::SetClientTravel("主机地址:7777?<登录串>")`，
   之后的游戏数据由引擎直连 UDP 7777。
4. 任何一个异常（连接被拒、5 秒无响应、包头非法、magic 不对）都只影响这次握手，
   不影响已经进图的会话。

握手只回答「能不能进」，没有密码、没有房间列表、不做身份认证；`status = 1`
的含义就是「主机进程在，但还没点创建多人游戏」，客户端可以过几秒再试。

---

## 2. EOS 隧道：帧格式（v1）

只用于 `force_ip_driver = 0`。每个 UDP 数据报恰好包含一帧。

### 帧头（51 字节，`#pragma pack(1)`）

| 偏移 | 大小 | 字段 | 说明 |
|-----:|-----:|------|------|
| 0 | 4 | magic | `0x54324E53`（ASCII `SN2T`） |
| 4 | 1 | version | 当前为 1 |
| 5 | 1 | type | 1 = DATA，2 = HELLO |
| 6 | 1 | channel | 透传 EOS P2P 的 channel |
| 7 | 1 | flags | 保留，必须为 0 |
| 8 | 4 | msgId | 发送方单调递增，标识一整条消息 |
| 12 | 2 | fragIndex | 分片序号，从 0 开始 |
| 14 | 2 | fragCount | 该消息的分片总数（1..128） |
| 16 | 33 | sender | 发送者 EOS PUID，32 个 hex 字符 + NUL |
| 49 | 2 | payloadLen | 本帧负载长度 |

帧总长 = 51 + payloadLen。分片时每片负载最多 `mtu - 51` 字节
（默认 mtu 1400 → 1349 字节负载；mtu 允许 512..2000，超出范围回落 1400）。

### 消息与分片

- 一条游戏数据包 = 一个 `msgId`，可能拆成 `fragCount` 帧。
- 接收端按 `sender + channel + msgId` 聚合，重复分片直接丢弃；5 秒未收齐则整条丢弃。
  同时在途消息最多 128 条，超出时丢弃新消息（防止伪造 msgId 撑爆内存）。
- 空负载不会进入隧道（`Tunnel::send` 直接返回 false，交回 EOS）。
- `fragCount > 128`、`payloadLen != 帧剩余长度`、`flags != 0` 都被视为非法帧。

### HELLO 与地址学习

- 客户端在还不知道主机直连是否可用时，每 2 秒发一个 HELLO（`msgId=0`，
  无负载，`sender` 为自己的 PUID）。
- 主机收到任何合法帧（包括 HELLO 和 DATA）都会记录 `sender → 源地址`，
  之后发往该 PUID 的包直接使用这个地址。地址变化时更新并记录日志。
- 主机收到 HELLO 后回一个 HELLO，`sender` 为主机自己的 PUID；主机的
  PUID 在它第一次调用 `SendPacket` 之前是未知的，此时不回复（客户端 2 秒后重试）。
- 客户端收到来自主机地址的任意帧即认为直连可用（`hostAlive`）。

### 与 EOS 的关系

- 控制面（登录、P2P 连接建立、`AcceptConnection`）仍走 EOS。
- 数据面：`EOS_P2P_SendPacket` 被拦截，负载原样放进上面的帧；
  `EOS_P2P_GetNextReceivedPacketSize` / `EOS_P2P_ReceivePacket` 从隧道队列
  取包，并用 `EOS_ProductUserId_FromString` 还原发送者句柄。
- 在某个 peer 的直连被证实之前，仍调用原版 `EOS_P2P_SendPacket`（引导回退）；
  `passthrough_until_seen = false` 可关闭该回退，完全依赖隧道。
- 隧道是不可靠 UDP；可靠/排序由 UE 的 NetDriver 自己负责。

---

## 安全与限制（两种模式）

- 握手和帧内 PUID 都没有签名或认证，任何知道主机地址的人都能发起连接/伪造发送者。
  只适合与朋友联机，不要暴露给不受信任的网络。
- 不支持 IPv6；没有加密（EOS 中继路径自带加密，直连路径没有）。
- 协议/包版本不匹配的帧会被静默丢弃；握手则明确返回 `status = 2`。
- IP 模式下 mod 只参与握手，游戏流量的可靠性/加密完全取决于引擎的 `IpNetDriver`。
