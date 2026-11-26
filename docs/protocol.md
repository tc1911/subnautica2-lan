# SN2LAN 线协议（v1）

所有整数小端序。每个 UDP 数据报恰好包含一帧。

## 帧头（51 字节，`#pragma pack(1)`）

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

## 消息与分片

- 一条游戏数据包 = 一个 `msgId`，可能拆成 `fragCount` 帧。
- 接收端按 `sender + channel + msgId` 聚合，重复分片直接丢弃；5 秒未收齐则整条丢弃。
  同时在途消息最多 128 条，超出时丢弃新消息（防止伪造 msgId 撑爆内存）。
- 空负载不会进入隧道（`Tunnel::send` 直接返回 false，交回 EOS）。
- `fragCount > 128`、`payloadLen != 帧剩余长度`、`flags != 0` 都被视为非法帧。

## HELLO 与地址学习

- 客户端在还不知道主机直连是否可用时，每 2 秒发一个 HELLO（`msgId=0`，
  无负载，`sender` 为自己的 PUID）。
- 主机收到任何合法帧（包括 HELLO 和 DATA）都会记录 `sender → 源地址`，
  之后发往该 PUID 的包直接使用这个地址。地址变化时更新并记录日志。
- 主机收到 HELLO 后回一个 HELLO，`sender` 为主机自己的 PUID；主机的
  PUID 在它第一次调用 `SendPacket` 之前是未知的，此时不回复（客户端 2 秒后重试）。
- 客户端收到来自主机地址的任意帧即认为直连可用（`hostAlive`）。

## 与 EOS 的关系

- 控制面（登录、P2P 连接建立、`AcceptConnection`）仍走 EOS。
- 数据面：`EOS_P2P_SendPacket` 被拦截，负载原样放进上面的帧；
  `EOS_P2P_GetNextReceivedPacketSize` / `EOS_P2P_ReceivePacket` 从隧道队列
  取包，并用 `EOS_ProductUserId_FromString` 还原发送者句柄。
- 在某个 peer 的直连被证实之前，仍调用原版 `EOS_P2P_SendPacket`（引导回退）；
  `passthrough_until_seen = false` 可关闭该回退，完全依赖隧道。
- 隧道是不可靠 UDP；可靠/排序由 UE 的 NetDriver 自己负责。

## 限制与安全

- 帧内 PUID 没有签名或认证，任何知道主机地址的人都能伪造发送者。
  只适合与朋友联机，不要暴露给不受信任的网络。
- 不支持 IPv6；没有加密（EOS 中继路径自带加密，直连路径没有）。
- 协议版本不匹配的帧会被静默丢弃。
