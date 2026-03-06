//

#pragma once

#include <cstdint>

int CreateListenSocket(unsigned short port, int backlog = 1024);
void SetNonBlocking(int fd);
