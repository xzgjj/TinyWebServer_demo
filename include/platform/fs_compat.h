// =============================================================================
// 文件系统兼容层
//
// 差异点：POSIX 是 mkdir(path, mode)，Windows 是 _mkdir(path)（无 mode）。
// 因为两者同名不同元数，宏重定义不安全（会误伤成员函数），这里用显式函数。
// =============================================================================
#ifndef TWS_PLATFORM_FS_COMPAT_H
#define TWS_PLATFORM_FS_COMPAT_H

#include <sys/stat.h>

#ifdef TWS_PLATFORM_WIN32
#include <direct.h>
#else
#include <sys/types.h>
#endif

namespace tws {

/// 创建目录：成功或"已存在"都返回 0，其余返回 -1。
inline int make_dir(const char* path, int mode) {
#ifdef TWS_PLATFORM_WIN32
    (void)mode;
    return ::_mkdir(path);
#else
    return ::mkdir(path, static_cast<mode_t>(mode));
#endif
}

}  // namespace tws

#endif  // TWS_PLATFORM_FS_COMPAT_H
