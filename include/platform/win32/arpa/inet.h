// <arpa/inet.h> 的 Windows 兼容实现：字节序与地址文本转换。
#ifndef TWS_WIN32_ARPA_INET_H
#define TWS_WIN32_ARPA_INET_H

#include <netinet/in.h>

#include <cstdint>
#include <cstring>

// 字节序转换：纯算术实现，避免依赖 <winsock2.h>。
// ws2_32 也导出 htons/htonl 等，static inline 具备内部链接，不会产生符号冲突。
static inline uint16_t htons(uint16_t v) {
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8)  | ((v & 0xff000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

extern "C" {
int tws_inet_pton(int af, const char* src, void* dst);
const char* tws_inet_ntop(int af, const void* src, char* dst, socklen_t size);
}

// ws2_32 也导出 inet_pton/inet_ntop：同名转发同样用 static inline 规避符号冲突
static inline int inet_pton(int af, const char* src, void* dst) {
    return tws_inet_pton(af, src, dst);
}
static inline const char* inet_ntop(int af, const void* src, char* dst, socklen_t size) {
    return tws_inet_ntop(af, src, dst, size);
}

#endif  // TWS_WIN32_ARPA_INET_H
