// =============================================================================
// Win32 后端接口（纯 Win32 一侧）
//
// 分层动机：POSIX ABI 实现（win32_compat.cpp）里不允许出现 winsock 类型，
// 否则会与自有 POSIX 结构定义冲突。因此把 winsock 全部关在本 TU 内，
// 对外只暴露 "uint64_t 原生句柄 + 纯标量参数" 的接口。
// =============================================================================
#ifndef TWS_PLATFORM_WIN32_BACKEND_H
#define TWS_PLATFORM_WIN32_BACKEND_H

#include <cstddef>
#include <cstdint>

namespace tws {
namespace win32 {

void backend_init();          // WSAStartup（幂等，线程安全）
int  last_error();            // 最近一次后端错误（0 = 无）
bool last_error_would_block();

// ---- 套接字 ---------------------------------------------------------------- //
uint64_t sock_create(bool stream, bool nonblocking);
int      sock_bind(uint64_t s, uint32_t ip_be, uint16_t port_be);
int      sock_listen(uint64_t s, int backlog);
uint64_t sock_accept(uint64_t s, bool nonblocking);
int      sock_connect(uint64_t s, uint32_t ip_be, uint16_t port_be);
int      sock_send(uint64_t s, const void* buf, int len, int flags);
int      sock_recv(uint64_t s, void* buf, int len, int flags);
int      sock_send_bufs(uint64_t s, const void* const* bufs, const int* lens, int count);
int      sock_recv_bufs(uint64_t s, void* const* bufs, const int* lens, int count);
int      sock_setopt_int(uint64_t s, int opt, int value);
int      sock_setopt_timeo(uint64_t s, int opt, int seconds);
int      sock_getopt_error(uint64_t s, int* out_value);
int      sock_local_addr(uint64_t s, uint32_t* ip_be, uint16_t* port_be);
int      sock_peer_addr(uint64_t s, uint32_t* ip_be, uint16_t* port_be);
int      sock_shutdown(uint64_t s, int how);
int      sock_close(uint64_t s);
int      sock_set_nonblocking(uint64_t s, bool on);
int      sock_poll(const uint64_t* handles, const short* events, short* revents,
                   int count, int timeout_ms);

// 自连 UDP 套接字（用作 eventfd 唤醒通道）；失败返回 0
uint64_t udp_self_pair(bool nonblocking);

// ---- 进程信息 -------------------------------------------------------------- //
std::size_t process_working_set_kb();

}  // namespace win32
}  // namespace tws

#endif  // TWS_PLATFORM_WIN32_BACKEND_H
