//

#pragma once

#include <cstdint>

int CreateListenSocket(std::uint16_t port);
void SetNonBlocking(int fd);
