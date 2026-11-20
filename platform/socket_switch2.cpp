#include <unistd.h>
#include <rex/net/socket.h>
namespace rex::net {
int socket_close(SocketHandle handle) { return close(static_cast<int>(handle)); }
int socket_ioctl(SocketHandle, uint32_t, uint8_t*) { return -1; }
}  // namespace rex::net
