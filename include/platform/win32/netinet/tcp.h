// <netinet/tcp.h> 的 Windows 兼容实现。
// TCP_NODELAY 受支持；TCP_CORK 为 Linux 专有，本层让 setsockopt 显式失败
// （返回 -1/ENOPROTOOPT），调用方按既有逻辑忽略即可，不做"假装成功"。
#ifndef TWS_WIN32_NETINET_TCP_H
#define TWS_WIN32_NETINET_TCP_H

#include <netinet/in.h>

#define TCP_NODELAY 1
#define TCP_CORK    3

#endif  // TWS_WIN32_NETINET_TCP_H
