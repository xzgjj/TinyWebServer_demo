// =============================================================================
// Win32 文件映射后端（静态资源零拷贝路径）
//
// 与套接字后端分开的原因同上：本 TU 需要 <windows.h>/<io.h>，而 POSIX ABI 实现
// 单元必须保持"无 windows 头"。对外只暴露裸指针 + 长度。
// =============================================================================
#ifndef TWS_PLATFORM_WIN32_FILEMAP_H
#define TWS_PLATFORM_WIN32_FILEMAP_H

#include <cstddef>

namespace tws {
namespace win32 {

// 只读映射一个 CRT 文件描述符；失败返回 nullptr（调用方需回退到普通读）
void* map_file_readonly(int crt_fd, std::size_t length);

// 解除映射；成功返回 0
int unmap_file(void* addr, std::size_t length);

}  // namespace win32
}  // namespace tws

#endif  // TWS_PLATFORM_WIN32_FILEMAP_H
