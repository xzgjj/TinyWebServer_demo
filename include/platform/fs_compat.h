// =============================================================================
// 文件系统兼容层
//
// 差异点：POSIX 是 mkdir(path, mode)，Windows 是 _mkdir(path)（无 mode）。
// 因为两者同名不同元数，宏重定义不安全（会误伤成员函数），这里用显式函数。
// =============================================================================
#ifndef TWS_PLATFORM_FS_COMPAT_H
#define TWS_PLATFORM_FS_COMPAT_H

#include <sys/stat.h>

// 判据用编译器自带的 _WIN32；本头文件只解决 mkdir 的元数差异，不依赖平台层。
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/types.h>
#endif

namespace tws {

/// 创建目录：成功返回 0，失败（含已存在）返回 -1。
inline int make_dir(const char* path, int mode) {
#ifdef _WIN32
    (void)mode;
    return ::_mkdir(path);
#else
    return ::mkdir(path, static_cast<mode_t>(mode));
#endif
}

}  // namespace tws

#endif  // TWS_PLATFORM_FS_COMPAT_H
