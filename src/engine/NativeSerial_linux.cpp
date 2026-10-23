// TCGETS2 for NativeSerial.cpp (Linux). struct termios2 comes from the kernel headers, which cannot be
// included together with glibc's <termios.h>, so this translation unit includes neither <termios.h>
// nor <sys/ioctl.h> and declares ioctl itself.

#include <asm/ioctls.h>
#include <asm/termbits.h>

#include <cerrno>
#include <cstdint>

extern "C" int ioctl(int fd, unsigned long request, ...) noexcept;

namespace asb::engine::detail {

bool linuxOutputRate(int fd, std::int32_t* rate, int* error)
{
    struct termios2 t {};
    if (::ioctl(fd, TCGETS2, &t) != 0) {
        *error = errno;
        return false;
    }
    *rate = static_cast<std::int32_t>(t.c_ospeed);
    return true;
}

}  // namespace asb::engine::detail
