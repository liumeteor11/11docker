#ifndef NETLINK_H
#define NETLINK_H

#include <stdint.h>

int nl_add_route(const char *dest, const char *gateway, const char *dev);

/**
 * 创建 veth pair（虚拟网卡对）
 * @param name1 第一个接口名称
 * @param name2 第二个接口名称（对端）
 * @return 0 成功，-1 失败
 */
int nl_create_veth(const char *name1, const char *name2);

/**
 * 设置接口状态为 UP
 * @param ifname 接口名称
 * @return 0 成功，-1 失败
 */
int nl_set_up(const char *ifname);

/**
 * 为接口添加 IPv4 地址
 * @param ifname 接口名称
 * @param ip_cidr IP/CIDR 格式，如 "10.0.0.1/24"
 * @return 0 成功，-1 失败
 */
int nl_add_addr(const char *ifname, const char *ip_cidr);

/**
 * 删除接口
 * @param ifname 接口名称
 * @return 0 成功，-1 失败
 */
int nl_del_link(const char *ifname);

/**
 * 将接口移动到指定网络命名空间
 * @param ifname 接口名称
 * @param ns_fd 网络命名空间的文件描述符
 * @return 0 成功，-1 失败
 */
int nl_move_to_ns(const char *ifname, int ns_fd);

/**
 * 重命名接口
 * @param oldname 原接口名称
 * @param newname 新接口名称
 * @return 0 成功，-1 失败
 */
int nl_rename_link(const char *oldname, const char *newname);

#endif