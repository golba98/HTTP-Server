#include "http/FileDescriptor.hpp"

#include <cerrno>
#include <utility>

#include <unistd.h>

namespace http {

namespace {

void closeDescriptor(int fd) noexcept
{
    if (fd < 0) {
        return;
    }

    // Preserve errno so cleanup on an error path cannot clobber the error
    // the caller is about to report.
    const int savedErrno = errno;

    // Never retry close(): Linux releases the descriptor even when close()
    // reports EINTR, so a retry could close an unrelated descriptor that has
    // reused the number. There is nobody to report a failure to here.
    static_cast<void>(::close(fd));

    errno = savedErrno;
}

// Every negative value means "owns nothing"; storing one canonical value keeps
// get() and comparisons predictable.
int normalize(int fd) noexcept
{
    return fd < 0 ? -1 : fd;
}

} // namespace

FileDescriptor::FileDescriptor(int fd) noexcept
    : fd_{normalize(fd)}
{
}

FileDescriptor::~FileDescriptor()
{
    reset();
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : fd_{std::exchange(other.fd_, -1)}
{
}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept
{
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int FileDescriptor::get() const noexcept
{
    return fd_;
}

bool FileDescriptor::valid() const noexcept
{
    return fd_ >= 0;
}

FileDescriptor::operator bool() const noexcept
{
    return valid();
}

int FileDescriptor::release() noexcept
{
    return std::exchange(fd_, -1);
}

void FileDescriptor::reset(int newFd) noexcept
{
    newFd = normalize(newFd);
    if (newFd == fd_) {
        return;
    }

    // Store the new value before closing so this object never holds a
    // descriptor that has already been closed.
    closeDescriptor(std::exchange(fd_, newFd));
}

} // namespace http
