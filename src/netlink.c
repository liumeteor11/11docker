#define _GNU_SOURCE
#include "netlink.h"
#include "log.h"
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef VETH_INFO_PEER
/* 旧内核可能未定义 VETH_INFO_PEER，这里手动定义为 1（Linux 标准值） */
#define VETH_INFO_PEER 1
#endif

/**
 * 创建并配置 NETLINK_ROUTE 套接字
 * @return 套接字文件描述符，失败返回 -1
 */
static int nl_socket(void) {
  int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (sock < 0) {
    log_error("socket: %m");
    return -1;
  }
  /* 设置接收超时 1 秒，防止某些内核行为导致 recvmsg 永久阻塞 */
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    log_warn("setsockopt SO_RCVTIMEO failed: %m");
    /* 不致命，继续使用 */
  }
  return sock;
}

/**
 * 发送 netlink 消息并等待预期的响应（ACK 或指定类型）
 * @param sock  netlink 套接字
 * @param nlh   待发送的 netlink 消息头
 * @param seq   期望的序列号
 * @param type  期望的 nlmsg_type（如 RTM_NEWLINK），若收到 NLMSG_DONE
 * 也视为成功
 * @return 0 成功，-1 失败
 */
static int nl_send_recv(int sock, struct nlmsghdr *nlh, uint32_t seq,
                        int type) {
  struct sockaddr_nl addr = {.nl_family = AF_NETLINK};
  struct iovec iov = {.iov_base = nlh, .iov_len = nlh->nlmsg_len};
  struct msghdr msg = {.msg_name = &addr,
                       .msg_namelen = sizeof(addr),
                       .msg_iov = &iov,
                       .msg_iovlen = 1};

  if (sendmsg(sock, &msg, 0) < 0) {
    log_error("sendmsg: %m");
    return -1;
  }

  char buf[4096];
  iov.iov_base = buf;
  iov.iov_len = sizeof(buf);
  int len = recvmsg(sock, &msg, 0);
  if (len < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      log_error("recvmsg timed out");
    } else {
      log_error("recvmsg: %m");
    }
    return -1;
  }

  struct nlmsghdr *resp = (void *)buf;
  while (NLMSG_OK(resp, len)) {
    if (resp->nlmsg_seq != seq) {
      resp = NLMSG_NEXT(resp, len);
      continue;
    }
    if (resp->nlmsg_type == NLMSG_ERROR) {
      struct nlmsgerr *err = NLMSG_DATA(resp);
      if (err->error) {
        log_error("Netlink error code: %d", err->error);
        return -1;
      }
      return 0; /* ACK */
    }
    if (resp->nlmsg_type == type || resp->nlmsg_type == NLMSG_DONE)
      return 0;
    resp = NLMSG_NEXT(resp, len);
  }
  return -1;
}

/**
 * 轮询等待网络接口出现（通过 if_nametoindex）
 * @param ifname     接口名称
 * @param timeout_ms 超时时间（毫秒）
 * @return 接口索引（>0）或 0（超时/不存在）
 */
static int wait_for_interface(const char *ifname, int timeout_ms) {
  int waited = 0;
  while (waited < timeout_ms) {
    int idx = if_nametoindex(ifname);
    if (idx != 0) {
      log_debug("interface %s ready (index=%d)", ifname, idx);
      return idx;
    }
    usleep(100000);
    waited += 100;
  }
  log_error("timed out waiting for interface: %s", ifname);
  return 0;
}

/**
 * 解析 CIDR 格式的 IPv4 地址，如 "192.168.1.1/24"
 * @param s    输入字符串
 * @param ip   输出网络字节序的 IPv4 地址
 * @param plen 输出前缀长度
 * @return 0 成功，-1 失败
 */
static int parse_cidr(const char *s, uint32_t *ip, uint8_t *plen) {
  char buf[64];
  if (strlen(s) >= sizeof(buf)) {
    log_error("IP/CIDR string too long: %s", s);
    return -1;
  }
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  char *slash = strchr(buf, '/');
  *plen = slash ? atoi(slash + 1) : 24;
  if (slash)
    *slash = 0;
  return inet_pton(AF_INET, buf, ip) == 1 ? 0 : -1;
}

/**
 * 创建 veth 虚拟网卡对
 * @param name1 一端接口名称（例如 "veth0"）
 * @param name2 另一端接口名称（例如 "veth1"）
 * @return 0 成功，-1 失败
 */
int nl_create_veth(const char *name1, const char *name2) {
  int sock = nl_socket();
  if (sock < 0)
    return -1;

  struct {
    struct nlmsghdr h;
    struct ifinfomsg i;
    char buf[256];
  } req = {0};

  req.h.nlmsg_len = NLMSG_LENGTH(sizeof(req.i));
  req.h.nlmsg_type = RTM_NEWLINK;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
  req.h.nlmsg_seq = 1;
  req.i.ifi_family = AF_UNSPEC; /* 不指定地址族 */

  /* 1. 主接口名称 (IFLA_IFNAME) */
  struct rtattr *iname = (void *)((char *)&req + NLMSG_ALIGN(req.h.nlmsg_len));
  iname->rta_type = IFLA_IFNAME;
  iname->rta_len = RTA_LENGTH(strlen(name1) + 1);
  memcpy(RTA_DATA(iname), name1, strlen(name1) + 1);
  req.h.nlmsg_len += RTA_ALIGN(iname->rta_len);

  /* 2. IFLA_LINKINFO 容器，用于描述链接类型为 veth */
  struct rtattr *li = (void *)((char *)&req + NLMSG_ALIGN(req.h.nlmsg_len));
  li->rta_type = IFLA_LINKINFO;
  li->rta_len = RTA_LENGTH(0);

  /* 2.1 IFLA_INFO_KIND = "veth" (4字节，不加空终止符) */
  struct rtattr *kind = (void *)((char *)li + RTA_LENGTH(0));
  kind->rta_type = IFLA_INFO_KIND;
  kind->rta_len = RTA_LENGTH(4);
  memcpy(RTA_DATA(kind), "veth", 4);
  li->rta_len += kind->rta_len;

  /* 2.2 IFLA_INFO_DATA + VETH_INFO_PEER，携带对端接口的信息 */
  struct rtattr *data = (void *)((char *)li + RTA_ALIGN(li->rta_len));
  data->rta_type = IFLA_INFO_DATA;
  data->rta_len = RTA_LENGTH(0);

  struct rtattr *peer = (void *)((char *)data + RTA_LENGTH(0));
  peer->rta_type = VETH_INFO_PEER;
  peer->rta_len = RTA_LENGTH(sizeof(req.i));
  memset(RTA_DATA(peer), 0, sizeof(req.i));

  /* 对等端名称（嵌套在 peer 内） */
  struct rtattr *pname = (void *)((char *)RTA_DATA(peer) + sizeof(req.i));
  pname->rta_type = IFLA_IFNAME;
  pname->rta_len = RTA_LENGTH(strlen(name2) + 1);
  memcpy(RTA_DATA(pname), name2, strlen(name2) + 1);
  peer->rta_len += RTA_ALIGN(pname->rta_len);

  data->rta_len += RTA_ALIGN(peer->rta_len);
  li->rta_len += RTA_ALIGN(data->rta_len);
  req.h.nlmsg_len += RTA_ALIGN(li->rta_len);

  int ret = nl_send_recv(sock, &req.h, req.h.nlmsg_seq, RTM_NEWLINK);
  close(sock);

  if (ret < 0)
    return -1;

  /* 等待接口真正出现在系统中（避免后续操作因接口尚未创建而失败） */
  log_debug("waiting for veth interfaces to appear...");
  int idx1 = wait_for_interface(name1, 5000);
  int idx2 = wait_for_interface(name2, 5000);

  if (idx1 == 0 || idx2 == 0) {
    log_error("veth pair creation incomplete: %s(idx=%d) <-> %s(idx=%d)", name1,
              idx1, name2, idx2);
    /* 尝试清理可能已创建的接口 */
    nl_del_link(name1);
    return -1;
  }

  log_info("veth pair ready: %s(index=%d) <-> %s(index=%d)", name1, idx1, name2,
           idx2);
  return 0;
}

/**
 * 将指定接口设置为 UP 状态
 * @param ifname 接口名称
 * @return 0 成功，-1 失败
 */
int nl_set_up(const char *ifname) {
  int sock = nl_socket();
  if (sock < 0)
    return -1;

  int idx = wait_for_interface(ifname, 5000);
  if (!idx) {
    log_error("no such interface: %s", ifname);
    close(sock);
    return -1;
  }

  struct {
    struct nlmsghdr h;
    struct ifinfomsg i;
  } req = {0};
  req.h.nlmsg_len = NLMSG_LENGTH(sizeof(req.i));
  req.h.nlmsg_type = RTM_NEWLINK;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  req.h.nlmsg_seq = 2;
  req.i.ifi_family = AF_UNSPEC;
  req.i.ifi_index = idx;
  req.i.ifi_change = IFF_UP; /* 仅修改 IFF_UP 标志 */
  req.i.ifi_flags = IFF_UP;  /* 设置 IFF_UP */

  int ret = nl_send_recv(sock, &req.h, req.h.nlmsg_seq, RTM_NEWLINK);
  close(sock);
  return ret;
}

/**
 * 为网络接口添加 IPv4 地址（CIDR 格式）
 * @param ifname  接口名称
 * @param ip_cidr 地址字符串，如 "192.168.1.2/24"
 * @return 0 成功，-1 失败
 */
int nl_add_addr(const char *ifname, const char *ip_cidr) {
  int sock = nl_socket();
  if (sock < 0)
    return -1;

  int idx = wait_for_interface(ifname, 5000);
  if (!idx) {
    log_error("timed out waiting for interface: %s", ifname);
    close(sock);
    return -1;
  }

  uint32_t ip;
  uint8_t plen;
  if (parse_cidr(ip_cidr, &ip, &plen) < 0) {
    close(sock);
    return -1;
  }

  struct {
    struct nlmsghdr h;
    struct ifaddrmsg a;
    char buf[64];
  } req = {0};
  req.h.nlmsg_len = NLMSG_LENGTH(sizeof(req.a));
  req.h.nlmsg_type = RTM_NEWADDR;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK;
  req.h.nlmsg_seq = 3;
  req.a.ifa_index = idx;
  req.a.ifa_family = AF_INET;
  req.a.ifa_prefixlen = plen;
  req.a.ifa_scope = RT_SCOPE_UNIVERSE; /* 全局范围 */

  /* IFA_LOCAL：本地地址（通常与 IFA_ADDRESS 相同） */
  struct rtattr *rta = (void *)((char *)&req + NLMSG_ALIGN(req.h.nlmsg_len));
  rta->rta_type = IFA_LOCAL;
  rta->rta_len = RTA_LENGTH(sizeof(ip));
  memcpy(RTA_DATA(rta), &ip, sizeof(ip));
  req.h.nlmsg_len += RTA_ALIGN(rta->rta_len);

  /* IFA_ADDRESS：对端地址（点对点链路时有效，普通以太网可与 IFA_LOCAL 相同） */
  rta = (void *)((char *)&req + NLMSG_ALIGN(req.h.nlmsg_len));
  rta->rta_type = IFA_ADDRESS;
  rta->rta_len = RTA_LENGTH(sizeof(ip));
  memcpy(RTA_DATA(rta), &ip, sizeof(ip));
  req.h.nlmsg_len += RTA_ALIGN(rta->rta_len);

  int ret = nl_send_recv(sock, &req.h, req.h.nlmsg_seq, RTM_NEWADDR);
  close(sock);
  return ret;
}

/**
 * 删除网络接口（如 veth 一端）
 * @param ifname 接口名称
 * @return 0 成功，-1 失败（接口不存在时也返回 0，仅警告）
 */
int nl_del_link(const char *ifname) {
  int sock = nl_socket();
  if (sock < 0)
    return -1;
  int idx = if_nametoindex(ifname);
  if (!idx) {
    log_warn("no such interface to delete: %s", ifname);
    close(sock);
    return 0;
  }

  struct {
    struct nlmsghdr h;
    struct ifinfomsg i;
  } req = {0};
  req.h.nlmsg_len = NLMSG_LENGTH(sizeof(req.i));
  req.h.nlmsg_type = RTM_DELLINK;
  req.h.nlmsg_flags = NLM_F_REQUEST;
  req.h.nlmsg_seq = 4;
  req.i.ifi_family = AF_UNSPEC;
  req.i.ifi_index = idx;

  int ret = nl_send_recv(sock, &req.h, req.h.nlmsg_seq, RTM_DELLINK);
  close(sock);
  return ret;
}

/**
 * 添加 IPv4 路由
 * @param dest    目标网络，格式 "192.168.1.0/24" 或 "default"（等价于
 * 0.0.0.0/0）
 * @param gateway 可选，网关地址（字符串形式），若为 NULL 则添加直连路由
 * @param dev     出口设备名称（如 "eth0"）
 * @return 0 成功，-1 失败
 */
int nl_add_route(const char *dest, const char *gateway, const char *dev) {
  int sock = nl_socket();
  if (sock < 0)
    return -1;

  struct {
    struct nlmsghdr h;
    struct rtmsg r;
    char attrbuf[512];
  } req = {0};

  req.h.nlmsg_len = NLMSG_LENGTH(sizeof(req.r));
  req.h.nlmsg_type = RTM_NEWROUTE;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
  req.h.nlmsg_seq = 5;
  req.r.rtm_family = AF_INET;
  req.r.rtm_table = RT_TABLE_MAIN;
  req.r.rtm_protocol = RTPROT_STATIC;
  req.r.rtm_type = RTN_UNICAST;

  /* 解析目的地址 / 前缀 */
  uint32_t dst_addr = 0; // 默认为 0.0.0.0
  uint8_t dst_plen = 0;

  if (dest && strcmp(dest, "default") != 0) {
    char buf[64];
    strncpy(buf, dest, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *slash = strchr(buf, '/');
    if (slash) {
      *slash = '\0';
      dst_plen = atoi(slash + 1);
    } else {
      dst_plen = 32; // 主机路由
    }
    if (inet_pton(AF_INET, buf, &dst_addr) != 1) {
      log_error("invalid destination address: %s", dest);
      close(sock);
      return -1;
    }
  }
  req.r.rtm_dst_len = dst_plen;

  /* scope: 有网关用 UNIVERSE（全网），直连路由用 LINK（链路本地） */
  if (gateway) {
    req.r.rtm_scope = RT_SCOPE_UNIVERSE;
  } else {
    req.r.rtm_scope = RT_SCOPE_LINK;
  }

  char *p = (char *)&req + NLMSG_ALIGN(req.h.nlmsg_len);

  /* 1. RTA_DST —— 目的网络地址（默认路由也建议带上全零） */
  {
    struct rtattr *rta = (void *)p;
    rta->rta_type = RTA_DST;
    rta->rta_len = RTA_LENGTH(sizeof(dst_addr));
    memcpy(RTA_DATA(rta), &dst_addr, sizeof(dst_addr));
    p += RTA_ALIGN(rta->rta_len);
  }

  /* 2. RTA_GATEWAY（若指定） */
  if (gateway) {
    struct rtattr *rta = (void *)p;
    rta->rta_len = RTA_LENGTH(sizeof(struct in_addr));
    rta->rta_type = RTA_GATEWAY;
    if (inet_pton(AF_INET, gateway, RTA_DATA(rta)) != 1) {
      log_error("invalid gateway address: %s", gateway);
      close(sock);
      return -1;
    }
    p += RTA_ALIGN(rta->rta_len);
  }

  /* 3. RTA_OIF —— 出口接口索引 */
  if (dev) {
    int dev_idx = if_nametoindex(dev);
    if (!dev_idx) {
      log_error("no such interface: %s", dev);
      close(sock);
      return -1;
    }
    struct rtattr *rta = (void *)p;
    rta->rta_len = RTA_LENGTH(sizeof(int));
    rta->rta_type = RTA_OIF;
    memcpy(RTA_DATA(rta), &dev_idx, sizeof(int));
    p += RTA_ALIGN(rta->rta_len);
  }

  req.h.nlmsg_len = p - (char *)&req;

  int ret = nl_send_recv(sock, &req.h, req.h.nlmsg_seq, RTM_NEWROUTE);
  close(sock);
  return ret;
}

/**
 * 将网络接口移动到指定的网络命名空间
 * @param ifname 接口名称
 * @param ns_fd  目标网络命名空间的文件描述符（通常由 open("/proc/<pid>/ns/net")
 * 或 setns 获得）
 * @return 0 成功，-1 失败
 */
int nl_move_to_ns(const char *ifname, int ns_fd) {
  int sock = nl_socket();
  if (sock < 0)
    return -1;

  int idx = if_nametoindex(ifname);
  if (!idx) {
    log_error("no such interface to move: %s", ifname);
    close(sock);
    return -1;
  }

  struct {
    struct nlmsghdr h;
    struct ifinfomsg i;
    char buf[64];
  } req = {0};

  req.h.nlmsg_len = NLMSG_LENGTH(sizeof(req.i));
  req.h.nlmsg_type = RTM_NEWLINK;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  req.h.nlmsg_seq = 6;
  req.i.ifi_family = AF_UNSPEC;
  req.i.ifi_index = idx;

  struct rtattr *rta = (void *)((char *)&req + NLMSG_ALIGN(req.h.nlmsg_len));
  rta->rta_type = IFLA_NET_NS_FD;
  rta->rta_len = RTA_LENGTH(sizeof(int));
  memcpy(RTA_DATA(rta), &ns_fd, sizeof(int));
  req.h.nlmsg_len += RTA_ALIGN(rta->rta_len);

  int ret = nl_send_recv(sock, &req.h, req.h.nlmsg_seq, RTM_NEWLINK);
  close(sock);
  if (ret < 0) {
    log_error("failed to move %s to netns", ifname);
  }
  return ret;
}

/**
 * 重命名网络接口
 * @param oldname 当前名称
 * @param newname 新名称
 * @return 0 成功，-1 失败
 */
int nl_rename_link(const char *oldname, const char *newname) {
  int sock = nl_socket();
  if (sock < 0)
    return -1;

  int idx = if_nametoindex(oldname);
  if (!idx) {
    log_error("no such interface to rename: %s", oldname);
    close(sock);
    return -1;
  }

  struct {
    struct nlmsghdr h;
    struct ifinfomsg i;
    char buf[64];
  } req = {0};

  req.h.nlmsg_len = NLMSG_LENGTH(sizeof(req.i));
  req.h.nlmsg_type = RTM_NEWLINK;
  req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  req.h.nlmsg_seq = 7;
  req.i.ifi_family = AF_UNSPEC;
  req.i.ifi_index = idx;

  struct rtattr *rta = (void *)((char *)&req + NLMSG_ALIGN(req.h.nlmsg_len));
  rta->rta_type = IFLA_IFNAME;
  rta->rta_len = RTA_LENGTH(strlen(newname) + 1);
  memcpy(RTA_DATA(rta), newname, strlen(newname) + 1);
  req.h.nlmsg_len += RTA_ALIGN(rta->rta_len);

  int ret = nl_send_recv(sock, &req.h, req.h.nlmsg_seq, RTM_NEWLINK);
  close(sock);
  if (ret < 0) {
    log_error("failed to rename %s to %s", oldname, newname);
  }
  return ret;
}