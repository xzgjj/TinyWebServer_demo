//

#include "common/error.h"
#include <cstring>

std::string ErrnoToString(int err)
{
    return std::string(std::strerror(err));
}
