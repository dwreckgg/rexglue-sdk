#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <sys/ioctl.h>

#include <cstdint>
#include <cstring>
#include <unistd.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

// The guest's Winsock FIONBIO: Linux's has another number and takes an int, so the guest's would fail and leave the
// socket blocking (upstream src/xam/socket_object.cpp catches it too, kFIONBIO). Any non-zero word turns it on, in
// either byte order.
constexpr uint32_t kWinsockFionbio = 0x8004667E;

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  if (cmd == kWinsockFionbio && arg) {
    uint32_t value = 0;
    std::memcpy(&value, arg, sizeof(value));
    int on = value != 0;
    return ioctl(static_cast<int>(handle), FIONBIO, &on);
  }
  return ioctl(static_cast<int>(handle), cmd, arg);
}

}  // namespace rex::net
