// <sys/resource.h> 的 Windows 兼容实现：getrusage。
// 只填 ru_maxrss（工作集 KB），其余保持 0 —— 明确"不完整的语义"而不是伪造数据。
#ifndef TWS_WIN32_SYS_RESOURCE_H
#define TWS_WIN32_SYS_RESOURCE_H

#include "tws_posix_types.h"

#include <sys/time.h>

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

extern "C" {
int getrusage(int who, struct rusage* usage);
}

#endif  // TWS_WIN32_SYS_RESOURCE_H
