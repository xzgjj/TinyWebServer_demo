// =============================================================================
// Win32 文件映射实现（本 TU 接触 windows.h / io.h）
// =============================================================================
#include "platform/win32_filemap.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>

#include <cstdint>
#include <map>
#include <mutex>

namespace tws {
namespace win32 {
namespace {

std::mutex g_mu;
// 记录"视图地址 -> 映射句柄"，供 munmap 释放；避免把文件句柄泄漏到调用方
std::map<void*, HANDLE> g_mappings;

}  // namespace

void* map_file_readonly(int crt_fd, std::size_t length) {
    if (crt_fd < 0 || length == 0) {
        return nullptr;
    }
    intptr_t os_handle = _get_osfhandle(crt_fd);
    if (os_handle == -1) {
        return nullptr;
    }
    HANDLE mapping = CreateFileMappingW(reinterpret_cast<HANDLE>(os_handle), nullptr,
                                        PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        return nullptr;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(mapping);
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_mappings[view] = mapping;
    return view;
}

int unmap_file(void* addr, std::size_t /*length*/) {
    if (addr == nullptr) {
        return -1;
    }
    HANDLE mapping = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_mappings.find(addr);
        if (it == g_mappings.end()) {
            return -1;
        }
        mapping = it->second;
        g_mappings.erase(it);
    }
    int rc = UnmapViewOfFile(addr) ? 0 : -1;
    if (mapping != nullptr) {
        CloseHandle(mapping);
    }
    return rc;
}

}  // namespace win32
}  // namespace tws
