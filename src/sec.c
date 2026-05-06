#include "sec.h"
#include "log.h"
#include <asm-generic/ioctls.h>
#include <linux/capability.h>
#include <linux/prctl.h>
#include <linux/sched.h>
#include <seccomp.h>
#include <stdio.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/stat.h>

/**
 * 配置进程的能力（Capabilities）
 *
 * Linux capabilities
 * 是一种细粒度的权限管理机制，用于精确控制进程可以执行哪些特权操作。
 * 传统的Unix只有root和非root两种权限级别，而capabilities将root的特权拆分为多个独立的能力单元。
 *
 * 本函数的工作流程：
 * 1. 首先清理进程自身的能力继承集（inheritable set）和边界集（bounding set）
 * 2. 然后有选择地丢弃不需要的危险能力，保留必要的安全能力
 *
 * 被丢弃的能力列表（共18个）：
 *
 * 审计相关能力（3个）：
 * - CAP_AUDIT_CONTROL: 控制内核审计系统，启用/禁用审计
 * - CAP_AUDIT_READ:   读取内核审计日志
 * - CAP_AUDIT_WRITE:  写入内核审计日志
 *
 * 系统控制相关能力（6个）：
 * - CAP_BLOCK_SUSPEND: 阻止系统进入休眠状态
 * - CAP_SYS_ADMIN:     执行各种系统管理任务（极其危险，相当于"万能"能力）
 * - CAP_SYS_BOOT:      重启或关闭系统
 * - CAP_SYS_MODULE:    加载/卸载内核模块（可直接修改内核行为）
 * - CAP_SYS_NICE:      提升进程优先级超过普通限制
 * - CAP_SYS_RESOURCE:  修改资源限制（如突破ulimit限制）
 *
 * 文件和权限相关能力（4个）：
 * - CAP_DAC_READ_SEARCH: 绕过文件/目录的读和搜索权限检查
 * - CAP_FSETID:          设置文件的SUID位而不清除
 * - CAP_MKNOD:           创建设备文件（mknod）
 * - CAP_SETFCAP:         为文件设置任意capabilities
 *
 * 安全相关能力（3个）：
 * - CAP_MAC_ADMIN:    配置强制访问控制（MAC）策略
 * - CAP_MAC_OVERRIDE: 覆盖MAC策略
 * - CAP_IPC_LOCK:     锁定共享内存（防止被交换到磁盘）
 *
 * 其他危险能力（2个）：
 * - CAP_SYSLOG:    读取和清除内核日志缓冲区（可能泄露敏感信息）
 * - CAP_SYS_RAWIO: 执行原始I/O操作（直接访问硬件）
 * - CAP_SYS_TIME:  修改系统时钟
 * - CAP_WAKE_ALARM: 设置唤醒闹钟
 *
 * 保留的能力列表（共15个，这些是在容器环境中仍然需要的）：
 *
 * 文件操作能力（4个）：
 * - CAP_DAC_OVERRIDE: 在挂载命名空间内绕过文件权限检查（容器内文件操作需要）
 * - CAP_FOWNER:           在容器内执行需要文件所有权验证的操作
 * - CAP_LEASE:            在容器内建立文件租约
 * - CAP_LINUX_IMMUTABLE:  在容器内设置文件的不可变属性（chattr +i）
 *
 * 网络和IPC能力（4个）：
 * - CAP_NET_ADMIN:        配置网络接口、路由表、防火墙规则（容器网络隔离需要）
 * - CAP_NET_BIND_SERVICE: 绑定到1024以下的特权端口
 * - CAP_NET_RAW:          使用原始套接字（ping、某些网络诊断工具需要）
 * - CAP_IPC_OWNER:        在命名空间内配置IPC对象
 *
 * 进程管理能力（5个）：
 * - CAP_SYS_PTRACE:       使用ptrace()调试进程（但不能跨PID命名空间）
 * - CAP_KILL:             向进程发送信号（但不能跨PID命名空间）
 * - CAP_SETUID:           设置任意用户ID（容器内用户切换需要）
 * - CAP_SETGID:           设置任意组ID
 * - CAP_SETPCAP:          修改进程的capabilities集合
 * - CAP_SYS_PACCT:        配置进程记账
 *
 * 其他能力（2个）：
 * - CAP_SYS_CHROOT:       执行chroot()（有安全风险，但在某些场景需要）
 * - CAP_SYS_TTY_CONFIG:   配置TTY设备（有安全风险，但终端操作需要）
 *
 * 安全注意事项：
 * - 某些能力可能无法完全隔离，因为它们不是命名空间化的（non-namespaced）
 * - 例如：向procfs的某些部分写入时，某些能力限制可能不会被完全尊重
 * - 边界集（bounding set）决定进程能获得的最大能力范围
 * - 继承集（inheritable set）决定execve()后哪些能力可以保留
 */
int sec_set_caps(void) {
  log_debug("setting capabilities...");
  int drop_caps[] = {
      CAP_AUDIT_CONTROL,   CAP_AUDIT_READ,   CAP_AUDIT_WRITE, CAP_BLOCK_SUSPEND,
      CAP_DAC_READ_SEARCH, CAP_FSETID,       CAP_IPC_LOCK,    CAP_MAC_ADMIN,
      CAP_MAC_OVERRIDE,    CAP_MKNOD,        CAP_SETFCAP,     CAP_SYSLOG,
      CAP_SYS_ADMIN,       CAP_SYS_BOOT,     CAP_SYS_MODULE,  CAP_SYS_NICE,
      CAP_SYS_RAWIO,       CAP_SYS_RESOURCE, CAP_SYS_TIME,    CAP_WAKE_ALARM};

  int num_caps = sizeof(drop_caps) / sizeof(*drop_caps);
  log_debug("dropping bounding capabilities...");
  for (int i = 0; i < num_caps; i++) {
    if (prctl(PR_CAPBSET_DROP, drop_caps[i], 0, 0, 0)) {
      log_error("failed to prctl cap %d: %m", drop_caps[i]);
      return 1;
    }
  }

  log_debug("dropping inheritable capabilities...");
  cap_t caps = NULL;
  if (!(caps = cap_get_proc()) ||
      cap_set_flag(caps, CAP_INHERITABLE, num_caps, drop_caps, CAP_CLEAR) ||
      cap_set_proc(caps)) {
    log_error("failed to run cap functions: %m");
    if (caps) {
      log_debug("freeing caps...");
      cap_free(caps);
    }

    return 1;
  }

  log_debug("freeing caps...");
  cap_free(caps);
  log_debug("capabilities set");

  return 0;
}

/**
 * 配置Seccomp（安全计算模式）系统调用过滤器
 *
 * Seccomp是一种内核安全机制，用于限制进程可以使用的系统调用。
 * 本函数基于Docker的默认seccomp配置文件，并额外添加了一些限制，
 * 用于阻止危险的或已废弃的系统调用。
 *
 * 配置策略说明：
 * - 默认策略（SCMP_ACT_ALLOW）：允许所有系统调用
 * - 特定规则（SCMP_ACT_ERRNO/SCMP_ACT_KILL）：拒绝特定的危险调用
 *
 * 与Docker默认策略的差异：
 *
 * Docker默认阻止但本代码允许的系统调用（5个）：
 * - get_mempolicy:    获取内存策略信息（可能泄露内存布局）
 * - getpagesize:      获取页面大小（可能辅助内存攻击）
 * - pciconfig_iobase: 获取PCI配置空间基址（泄露硬件信息）
 * - ustat:            获取文件系统统计信息（已废弃）
 * - sysfs:            获取文件系统类型信息（已废弃）
 * - uselib:           加载用户空间共享库（已废弃，安全风险）
 *
 * 这些调用被允许是因为：
 * - 它们要么已经被capabilities限制
 * - 要么只在特定架构上可用
 * - 要么是其他系统调用的别名或新版本
 *
 * 本函数阻止的系统调用分类说明：
 *
 * 1. 设置UID/GID位相关（6个规则）
 *    阻止chmod/fchmod/fchmodat设置S_ISUID和S_ISGID位
 *    原因：防止容器内创建setuid/setgid可执行文件，避免普通用户通过此类
 *          文件在宿主机上获得root权限（特别是在没有用户命名空间的情况下）
 *
 * 2. 创建新用户命名空间（2个规则）
 *    阻止unshare和clone带有CLONE_NEWUSER标志
 *    原因：防止容器内的进程创建新的用户命名空间并可能获得新的capabilities
 *
 * 3. 控制终端操作（1个规则）
 *    阻止ioctl的TIOCSTI操作
 *    原因：允许向控制终端注入字符，可能被用于权限提升攻击
 *
 * 4. 内核密钥环操作（3个规则）
 *    阻止keyctl、add_key、request_key
 *    原因：内核密钥环系统不是命名空间化的，容器内操作会影响宿主机
 *
 * 5. ptrace调试（1个规则）
 *    阻止ptrace系统调用
 *    原因：在Linux 4.8之前，ptrace会破坏seccomp的保护机制
 *
 * 6. NUMA内存策略（4个规则）
 *    阻止mbind、migrate_pages、move_pages、set_mempolicy
 *    原因：允许进程将内存绑定到特定NUMA节点，可能被用于拒绝服务攻击，
 *          影响主机上其他NUMA感知的应用程序
 *
 * 7. 用户空间页错误处理（1个规则）
 *    阻止userfaultfd
 *    原因：允许用户空间处理页错误，可被用于在内核中暂停执行（通过触发
 *          系统调用中的页错误），这是内核漏洞利用的常见技术
 *
 * 8. 性能事件监控（1个规则）
 *    阻止perf_event_open
 *    原因：可能泄露大量主机信息，理论上可用于发现内核地址和未初始化内存
 *
 * 9. 其他安全设置
 *    设置SCMP_FLTATR_CTL_NNP属性为0
 *    作用：阻止setuid和setcap二进制文件以额外特权执行
 *    副作用：由于奇怪的副作用，非特权用户将无法在容器内使用ping命令
 *          （因为ping通常是setuid root的）
 *
 * 维护说明：
 * - Linux内核会不断添加新的系统调用
 * - 此列表应定期更新以包含新发现的有问题的系统调用
 * - 建议关注Docker、containerd等项目的seccomp策略更新
 */
int sec_set_seccomp(void) {
  scmp_filter_ctx ctx = NULL;

  log_debug("setting syscalls...");
  if (!(ctx = seccomp_init(SCMP_ACT_ALLOW)) ||
      // 阻止设置SUID/SGID位的文件权限修改操作
      // 防止容器内创建特权可执行文件，避免权限提升漏洞
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(chmod), 1,
                       SCMP_A1(SCMP_CMP_MASKED_EQ, S_ISUID, S_ISUID)) ||
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(chmod), 1,
                       SCMP_A1(SCMP_CMP_MASKED_EQ, S_ISGID, S_ISGID)) ||
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(fchmod), 1,
                       SCMP_A1(SCMP_CMP_MASKED_EQ, S_ISUID, S_ISUID)) ||
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(fchmod), 1,
                       SCMP_A1(SCMP_CMP_MASKED_EQ, S_ISGID, S_ISGID)) ||
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(fchmodat), 1,
                       SCMP_A2(SCMP_CMP_MASKED_EQ, S_ISUID, S_ISUID)) ||
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(fchmodat), 1,
                       SCMP_A2(SCMP_CMP_MASKED_EQ, S_ISGID, S_ISGID)) ||

      // 阻止创建新的用户命名空间
      // 防止容器内进程通过新的用户命名空间获取额外权限
      seccomp_rule_add(
          ctx, SEC_SCMP_FAIL, SCMP_SYS(unshare), 1,
          SCMP_A0(SCMP_CMP_MASKED_EQ, CLONE_NEWUSER, CLONE_NEWUSER)) ||
      seccomp_rule_add(
          ctx, SEC_SCMP_FAIL, SCMP_SYS(clone), 1,
          SCMP_A0(SCMP_CMP_MASKED_EQ, CLONE_NEWUSER, CLONE_NEWUSER)) ||

      // 阻止向控制终端注入字符
      // 防止通过伪造终端输入进行权限提升攻击
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(ioctl), 1,
                       SCMP_A1(SCMP_CMP_MASKED_EQ, TIOCSTI, TIOCSTI)) ||

      // 阻止内核密钥环操作
      // 内核密钥环不是命名空间化的，容器内操作会影响宿主机
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(keyctl), 0) ||
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(add_key), 0) ||
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(request_key), 0) ||

      // 阻止ptrace调试（Linux 4.8之前会破坏seccomp）
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(ptrace), 0) ||

      // 阻止NUMA内存策略配置
      // 防止通过内存绑定进行拒绝服务攻击
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(mbind), 0) ||
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(migrate_pages), 0) ||
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(move_pages), 0) ||
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(set_mempolicy), 0) ||

      // 阻止用户空间页错误处理
      // 防止利用页错误机制进行内核漏洞利用
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(userfaultfd), 0) ||

      // 阻止性能事件监控
      // 防止泄露内核地址和敏感信息
      seccomp_rule_add(ctx, SEC_SCMP_FAIL, SCMP_SYS(perf_event_open), 0) ||

      // 设置no_new_privs标志，阻止setuid/setcap二进制文件获得特权
      // 这会导致ping等setuid程序无法工作，但增强了安全性
      seccomp_attr_set(ctx, SCMP_FLTATR_CTL_NNP, 0) || seccomp_load(ctx)) {

    log_error("failed to set syscalls: %m");

    // 释放seccomp上下文
    log_debug("releasing seccomp context...");
    if (ctx) {
      seccomp_release(ctx);
    }

    return 1;
  }

  log_debug("releasing seccomp context...");
  seccomp_release(ctx);
  log_debug("syscalls set");

  return 0;
}