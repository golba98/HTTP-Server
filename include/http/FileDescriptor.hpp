#pragma once

namespace http {

// Move-only owner of a POSIX file descriptor. Any negative value means the
// object owns nothing; the default state is -1. An owned descriptor is closed
// exactly once, when the owner is destroyed, reset, or move-assigned over.
class FileDescriptor {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept;

    ~FileDescriptor();

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept;
    FileDescriptor& operator=(FileDescriptor&& other) noexcept;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

    // Gives up ownership without closing. The caller must close the result.
    [[nodiscard]] int release() noexcept;

    // Closes the owned descriptor (if any) and takes ownership of newFd.
    // Passing the currently owned descriptor is a no-op.
    void reset(int newFd = -1) noexcept;

private:
    int fd_{-1};
};

} // namespace http
