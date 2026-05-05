# barco 容器运行时 — 代码架构思维导图

> **barco** 是一个用 C 语言编写的 Linux 容器运行时（类似简化版 Docker/runc），约 1500 行代码，适合快速理解容器底层原理。

---

## 项目全景鸟瞰图

```
                                barco (容器运行时)
                                     │
        ┌────────────────────────────┼────────────────────────────┐
        │                            │                            │
   构建系统                     核心源码                      第三方库
   (Makefile)                 (src/ + include/)              (lib/)
        │                            │                            │
   ├─ clang-18 编译              ├─ barco.c  主入口          ├─ argtable3  命令行解析
   ├─ clang-tidy 代码检查        ├─ container.c 容器生命周期   ├─ log.c      日志系统
   ├─ clang-format 格式化        ├─ mount.c    文件系统隔离
   ├─ valgrind 内存检查          ├─ cgroups.c  资源限制
   ├─ CUnit 单元测试             ├─ netlink.c  网络管理
   ├─ debug=1 调试模式           ├─ sec.c      安全加固
   └─ setup 一键安装依赖         └─ user.c     用户命名空间
```

---

## 主函数 (barco.c) 执行流程 — 从命令到容器

```
                         main() 主函数执行流程
                                  │
                  ┌───────────────┴───────────────┐
                  │                               │
            1. 解析命令行参数                  2. 填充配置结构体
            (argtable 库)                     (container_config)
                  │                               │
    ┌─ --help      显示帮助          ┌─ hostname   → "barcontainer"
    ├─ -u <n>      容器内 UID/GID     ├─ uid        → 用户指定的 UID
    ├─ -m <path>   根文件系统路径      ├─ mnt        → 根文件系统路径
    ├─ -c <cmd>    要执行的命令        ├─ cmd        → 要执行的命令
    ├─ -a <arg>    命令参数           ├─ arg        → 命令参数
    ├─ -v          详细输出           ├─ dns_server → DNS 服务器
    ├─ --veth      容器网卡名         ├─ veth_name  → 容器端网卡名
    ├─ --veth-peer 宿主机网卡名       ├─ veth_peer  → 宿主机端网卡名
    ├─ --container-ip 容器 IP/CIDR   ├─ container_ip → 容器 IP
    ├─ --host-ip   宿主机 IP/CIDR    ├─ host_ip    → 宿主机 IP
    ├─ --nat       启用 NAT 上网     └─ enable_nat → NAT 开关
    └─ --dns       DNS 服务器地址
                  │
                  └───────────────┬───────────────┐
                                  │               │
                  ┌───────────────┴───────────────┐
                  │                               │
            3. 宿主机侧网络配置               4. 创建容器进程
            (netlink 操作)                  (clone() 系统调用)
                  │                               │
    ┌─ 清理残留网卡                      ┌─ 分配 1MB 栈空间
    ├─ 创建 veth pair                    ├─ socketpair 创建通信通道
    ├─ 配置宿主机端 IP                   ├─ clone() 创建子进程
    ├─ 启用宿主机端网卡                   │   ├─ CLONE_NEWNS   挂载命名空间
    ├─ 启用 IP 转发 (/proc/sys/...)       │   ├─ CLONE_NEWCGROUP cgroup 命名空间
    └─ 配置 NAT iptables 规则            │   ├─ CLONE_NEWPID   PID 命名空间
        ├─ MASQUERADE (源地址伪装)        │   ├─ CLONE_NEWIPC   IPC 命名空间
        └─ FORWARD (允许转发)            │   ├─ CLONE_NEWNET   网络命名空间
                  │                       │   └─ CLONE_NEWUTS   主机名命名空间
                  │                       │
                  └───────────────┬───────┘
                                  │
                  ┌───────────────┴───────────────┐
                  │                               │
            5. 移动网卡到容器                6. 配置 cgroups
            (nl_move_to_ns)               (资源限制)
                  │                               │
            7. 配置用户命名空间映射         8. 等待容器退出
            (UID/GID 映射)                 (waitpid)
                  │                               │
                  └───────────────┬───────────────┘
                                  │
                            9. 清理资源
                       ├─ 删除 NAT 规则
                       ├─ 删除 veth 网卡
                       ├─ 释放 cgroups
                       └─ 释放内存
```

---

## 容器进程内部启动流程 (container_start)

```
                    container_start() — 子进程入口
                    (由 clone() 调用，运行在新命名空间中)
                                  │
              ┌───────────────────┴───────────────────┐
              │                                       │
    阶段 1：基础隔离                           阶段 2：网络配置
              │                                       │
    ├─ sethostname("barcontainer")         ├─ 启用 lo 回环网卡
    │   (设置容器主机名)                     │   (if_nametoindex + RTM_NEWLINK)
    │                                     ├─ 等待宿主机移入的 veth 网卡
    └─ mount_set()                        │   (轮询 if_nametoindex, 最长 5 秒)
        (切换根文件系统)                    ├─ 重命名网卡 (临时名 → 用户指定名)
        │                                 │   (nl_rename_link)
        ├─ MS_PRIVATE 挂载 /               ├─ 配置容器 IP 地址
        ├─ bind mount 新根目录              │   (nl_add_addr)
        ├─ pivot_root 切换根               ├─ 启用容器端网卡
        ├─ umount 旧根                     │   (nl_set_up)
        └─ mount /proc                    └─ 添加默认路由
              │                               (nl_add_route, 网关=宿主机 IP)
              │                               │
              │                         ┌──────┴──────┐
              │                         │             │
              │                    写入 DNS 配置   启用 ping 支持
              │                    (/etc/resolv.conf) (ping_group_range)
              │
              └───────────────────┬───────────────────┐
                                  │                   │
                        user_namespace_init()     设置 capabilities
                        (创建用户命名空间)           (sec_set_caps)
                                  │                   │
                        ├─ unshare(CLONE_NEWUSER)  ├─ 移除 18 个危险能力
                        ├─ 等待父进程写入映射          │   (CAP_SYS_ADMIN, ...)
                        ├─ setresuid/setresgid      └─ 保留 15 个必要能力
                                  │
                          sec_set_seccomp()
                          (系统调用过滤)
                                  │
                        ├─ 默认：允许所有系统调用
                        ├─ 阻止 chmod SUID/SGID
                        ├─ 阻止创建新用户命名空间
                        ├─ 阻止 TIOCSTI 终端注入
                        ├─ 阻止内核密钥环操作
                        ├─ 阻止 ptrace 调试
                        ├─ 阻止 NUMA 内存操作
                        ├─ 阻止 userfaultfd
                        └─ 阻止 perf_event_open
                                  │
                          execve(cmd)
                          (执行用户命令，如 /bin/bash)
```

---

## 文件系统隔离 (mount.c)

```
                        mount_set() — 切换根文件系统
                                  │
                    (目标：将容器限制在指定目录中)
                                  │
              ┌───────────────────┼───────────────────┐
              │                   │                   │
    1. 重新挂载 /             2. 绑定挂载            3. pivot_root
    mount(/)                  mount(mnt → tmp)       pivot_root(tmp, old)
    MS_REC | MS_PRIVATE            │                      │
         │                   ├─ 创建临时目录          ├─ 新根 = 临时目录
         目的：                   /tmp/barco.XXXXXX     ├─ 旧根 = tmp/oldroot
         防止容器内的挂载          ├─ MS_BIND 方式挂载    └─ 效果：
         操作泄漏到宿主机          │   将用户指定的        容器只能看到
                                  │   目录绑定到临时目录    指定目录中的文件
                                  │
              ┌───────────────────┼───────────────────┐
              │                                       │
    4. 卸载旧根                           5. 挂载 /proc
    umount2(old, MNT_DETACH)             mount("proc", "/proc")
         │                                      │
         清理旧根文件系统                          让 ps、top 等命令可用
         删除临时目录
```

**关键概念解释：**
- **pivot_root**：Linux 系统调用，把当前进程的根目录切换到新目录（比 chroot 更安全，无法轻易逃逸）
- **MS_PRIVATE**：阻止挂载事件在命名空间间传播
- **bind mount**：把目录"镜像"到另一个位置

---

## 网络子系统 (netlink.c)

```
                    netlink.c — Linux Netlink 协议通信
                                  │
              (通过 AF_NETLINK socket 与内核通信，配置网络)
                                  │
    ┌─────────────┬───────────────┼───────────────┬─────────────┐
    │             │               │               │             │
nl_create_veth  nl_set_up   nl_add_addr    nl_del_link   nl_move_to_ns
(创建虚拟网卡对)  (启用网卡)   (添加 IP)      (删除网卡)    (移动网卡到命名空间)
    │             │               │               │             │
    原理：         原理：          原理：          原理：        原理：
    RTM_NEWLINK   RTM_NEWLINK    RTM_NEWADDR    RTM_DELLINK   RTM_NEWLINK
    +             +              +              +             +
    IFLA_LINKINFO 设置 IFF_UP    设置 IFA_LOCAL  按 ifindex    设置
    +             标志            IFA_ADDRESS    删除          IFLA_NET_NS_FD
    VETH_INFO_PEER               和前缀长度                     传入目标 netns
                                                              的 fd

    结果：
    创建一对"管道"
    ┌─────────┐    ┌─────────┐
    │  veth0  │<──>│ veth1   │
    │(容器内) │    │(宿主机)  │
    └─────────┘    └─────────┘
    数据从一端进，另一端出

    其他函数：
    ├─ nl_add_route()   添加路由表条目 (RTM_NEWROUTE)
    └─ nl_rename_link() 重命名网卡 (RTM_NEWLINK + IFLA_IFNAME)
```

**网络拓扑示意：**

```
    容器内部                        宿主机
    ┌──────────────┐            ┌──────────────┐
    │  eth0        │   veth     │ veth-eth0-xx │
    │ 10.0.0.2/24  │<──────────>│ 10.0.0.1/24  │
    │              │   pair     │              │
    │  lo (回环)    │            │  eth0/wlan0  │──→ 互联网
    │              │            │  (NAT 伪装)   │
    │  DNS: 8.8.8.8│            │              │
    └──────────────┘            └──────────────┘
```

---

## 资源控制 (cgroups.c)

```
                    cgroups v2 — 限制容器资源使用
                                  │
                    cgroup 目录：/sys/fs/cgroup/<hostname>/
                                  │
              ┌───────────────────┼───────────────────┐
              │                   │                   │
         memory.max           cpu.weight           pids.max
         (内存上限)            (CPU 权重)           (进程数上限)
              │                   │                   │
         值: "1G"            值: "256"            值: "64"
         限制容器最多          相对权重值             限制容器内最多
         使用 1GB 内存         默认=100              64 个进程/线程
                              256≈1/4 核
              │                   │                   │
              └───────────────────┼───────────────────┘
                                  │
                          cgroup.procs
                         (进程控制文件)
                                  │
                    写入容器 PID，将此进程加入 cgroup
                    之后该进程及其所有子进程受上述限制
```

**关键概念解释：**
- **cgroups v2**：Linux 内核的控制组机制第 2 版，统一了 v1 的多个子系统
- **OOM Killer**：当内存超限时，内核会杀掉容器内进程
- **cpu.weight**：不是绝对限制，而是"权重"。如果有两个 cgroup 权重分别为 100 和 256，CPU 时间按 100:256 分配

---

## 安全子系统 (sec.c)

```
                    sec.c — 安全加固（两层防护）
                                  │
        ┌─────────────────────────┴─────────────────────────┐
        │                                                   │
  sec_set_caps()                                     sec_set_seccomp()
  (能力限制 — 第 1 层)                               (系统调用过滤 — 第 2 层)
        │                                                   │
  Linux Capabilities                                Seccomp (安全计算模式)
  将 root 特权拆成多个独立能力                       过滤进程可以调用的系统调用
        │                                                   │
  ┌─────┴─────┐                                     ┌─────┴─────┐
  │           │                                     │           │
移除 18 个   保留 15 个                           默认动作     黑名单规则
危险能力     必要能力                             ALLOW        (阻止特定调用)
  │           │                                   (允许所有)     │
  ├─ CAP_SYS_ADMIN    ├─ CAP_NET_ADMIN                    ├─ chmod 设置 SUID/SGID 位
  │   (万能管理)       │   (网络配置)                      ├─ unshare/clone NEWUSER
  ├─ CAP_SYS_BOOT     ├─ CAP_NET_RAW                      │   (禁止创建新用户命名空间)
  │   (重启系统)       │   (原始套接字)                    ├─ ioctl TIOCSTI
  ├─ CAP_SYS_MODULE   ├─ CAP_NET_BIND_SERVICE              │   (禁止向终端注入字符)
  │   (内核模块)       │   (绑定特权端口)                   ├─ keyctl/add_key/request_key
  ├─ CAP_SYS_RAWIO    ├─ CAP_DAC_OVERRIDE                  │   (禁止内核密钥环操作)
  │   (直接硬件访问)   │   (绕过文件权限)                   ├─ ptrace
  ├─ CAP_SYS_TIME     ├─ CAP_SETUID/GID                    │   (禁止调试)
  │   (修改系统时钟)   │   (切换用户)                       ├─ mbind/migrate_pages/...
  ├─ CAP_MAC_ADMIN    ├─ CAP_KILL                          │   (禁止 NUMA 操作)
  ├─ CAP_MAC_OVERRIDE │   (发送信号)                        ├─ userfaultfd
  ├─ CAP_AUDIT_*      ├─ CAP_SYS_PTRACE                    │   (禁止用户空间页错误处理)
  ├─ CAP_SYSLOG       ├─ CAP_SYS_CHROOT                    └─ perf_event_open
  ├─ CAP_MKNOD        └─ CAP_SYS_TTY_CONFIG                   (禁止性能监控)
  └─ ...
```

**最小权限原则（Principle of Least Privilege）：**
- Capabilities：只给容器运行必需的权限，不用 root 的所有特权
- Seccomp：即使有某个能力，也不能调用危险的系统调用
- **双重防护**：两道防线，任一都能阻止大多数攻击

---

## 用户命名空间 (user.c)

```
                user_namespace — UID/GID 隔离与映射
                                  │
              (让容器内的 root 在宿主机上只是普通用户)
                                  │
    ┌─────────────────────────────┴─────────────────────────────┐
    │                                                           │
子进程调用 (容器内)                                    父进程调用 (宿主机)
user_namespace_init()                            user_namespace_prepare_mappings()
    │                                                           │
    ├─ unshare(CLONE_NEWUSER)                    ├─ 等待子进程完成 unshare
    │   创建独立的用户命名空间                      │   (通过 socketpair 通信)
    │                                           ├─ 写入 deny 到 setgroups
    ├─ 通知父进程 "我准备好了"                     │   (禁止修改组列表，安全要求)
    │   (通过 socket 发送 0)                       ├─ 写入 uid_map
    │                                           │   /proc/<pid>/uid_map
    ├─ 等待父进程完成映射配置                       │   内容: "0 10000 1"
    │   (通过 socket 接收结果)                     │   含义: 容器内 UID 0
    │                                           │         = 宿主机 UID 10000
    ├─ setresuid(uid, uid, uid)                ├─ 写入 gid_map
    │   setresgid(uid, uid, uid)                │   内容: "0 10000 1"
    │   切换到目标用户                            │   含义: 容器内 GID 0
    │                                           │         = 宿主机 GID 10000
    └─ 完成                                      ├─ 通知子进程 "映射完成"
                                                    │
                                                  映射效果：
                                                  容器内 root (UID 0)
                                                  = 宿主机普通用户 (UID 10000)
                                                  即使逃逸出容器，也只有普通用户权限
```

**通信机制：socketpair**
```
  父进程 (宿主机)                    子进程 (容器)
  ┌──────────┐                    ┌──────────┐
  │ socket[0]│<─── 双向通信 ────>│ socket[1]│
  └──────────┘                    └──────────┘
  
  通信步骤：
  1. 子进程 unshare 后 send 0 → 父进程
  2. 父进程 配置映射后 send 0 → 子进程
  3. 子进程 收到成功信号，继续执行
```

---

## 项目文件结构与职责

```
bar/  (项目根目录)
│
├─ Makefile             构建系统：编译、测试、检查、安装
│   ├─ make             编译项目 → bin/barco
│   ├─ make test        运行 CUnit 测试
│   ├─ make lint        clang-tidy 静态分析
│   ├─ make format      clang-format 格式化代码
│   ├─ make check       valgrind 内存泄漏检查
│   ├─ make setup       一键安装所有依赖
│   └─ make clean       清理构建产物
│
├─ include/             头文件 (.h) — 接口定义
│   ├─ container.h      容器配置结构体 + API 声明
│   ├─ cgroups.h        资源限制常量 + API 声明
│   ├─ mount.h          文件系统隔离 API 声明
│   ├─ netlink.h        网络管理 API 声明
│   ├─ net_prio.h       网络优先级 (声明但未使用)
│   ├─ sec.h            安全加固 API 声明
│   └─ user.h           用户命名空间 API 声明
│
├─ src/                 源文件 (.c) — 功能实现
│   ├─ barco.c          ★ 主入口：参数解析、流程编排、资源清理
│   ├─ container.c      ★ 容器生命周期：clone、子进程启动、等待
│   ├─ mount.c          文件系统隔离：pivot_root 实现
│   ├─ cgroups.c        资源控制：cgroup v2 设置
│   ├─ netlink.c        网络：Netlink 协议通信
│   ├─ sec.c            安全：capabilities + seccomp
│   └─ user.c           用户命名空间：UID/GID 映射
│
├─ lib/                 第三方库
│   ├─ argtable/        命令行参数解析库 (argtable3)
│   └─ log/             日志库 (log.c by rxi)
│
├─ tests/               测试
│   └─ barco_test.c     CUnit 单元测试框架
│
├─ bin/                 编译产物 (make 后生成)
│   └─ barco            可执行文件
│
├─ build/               中间产物 (.o 文件)
│
├─ .clang-format        代码风格配置
├─ .clang-tidy          静态分析配置
└─ .vscode/             VSCode 开发配置
```

---

## 核心技术栈速查表

```
   概念                │  用什么实现              │  作用
───────────────────────┼─────────────────────────┼──────────────────────
   命名空间隔离         │  clone() + CLONE_NEW*    │  隔离进程视野
     ├─ 挂载命名空间     │  CLONE_NEWNS             │  隔离文件系统
     ├─ PID 命名空间     │  CLONE_NEWPID            │  隔离进程 ID
     ├─ 网络命名空间     │  CLONE_NEWNET            │  隔离网络栈
     ├─ UTS 命名空间     │  CLONE_NEWUTS            │  隔离主机名
     ├─ IPC 命名空间     │  CLONE_NEWIPC            │  隔离进程间通信
     ├─ Cgroup 命名空间  │  CLONE_NEWCGROUP         │  隔离 cgroup 视图
     └─ 用户命名空间     │  unshare(CLONE_NEWUSER)  │  隔离 UID/GID
───────────────────────┼─────────────────────────┼──────────────────────
   根文件系统切换        │  pivot_root()            │  限制文件访问范围
   资源限制              │  cgroups v2              │  CPU/内存/进程数
   安全加固              │  capabilities + seccomp  │  最小权限 + 系统调用过滤
   网络隔离              │  veth pair + netlink     │  独立网络栈 + NAT
───────────────────────┼─────────────────────────┼──────────────────────
   用户权限隔离          │  user namespace          │  root 容器 ≠ root 宿主机
```

---

## 新手学习路线建议

```
第 1 步：理解"容器是什么"
  → 阅读 barco.c 的 main() 函数，理解整体流程
  → 理解 clone() 和命名空间的概念

第 2 步：理解每个子系统
  → container.c  → 容器怎么"生"出来的
  → mount.c      → 容器怎么"看到"自己的文件系统
  → user.c       → 容器里的 root 为什么不是真 root
  → sec.c        → 怎么防止容器搞破坏
  → cgroups.c    → 怎么限制容器"吃"多少资源
  → netlink.c    → 网络怎么连接容器和外部

第 3 步：自己试着跑起来
  → 准备一个 rootfs（可以用 docker export 导出）
  → 用 sudo ./bin/barco -u 0 -m <rootfs> -c /bin/bash -a "-i" -v 启动
  → 在容器里运行 ps、ls、ping 等命令观察效果
```
