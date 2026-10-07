#include "http/FileDescriptor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <optional>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

using http::FileDescriptor;

// Note: POSIX hands out the lowest free descriptor number, so a test must not
// create new descriptors between closing one and checking that it is closed.

namespace {

// Returns {read end, write end}. The caller owns both descriptors.
std::array<int, 2> makePipe()
{
    std::array<int, 2> fds{-1, -1};
    REQUIRE(::pipe(fds.data()) == 0);
    return fds;
}

bool isOpen(int fd)
{
    errno = 0;
    return ::fcntl(fd, F_GETFD) != -1;
}

bool isClosed(int fd)
{
    errno = 0;
    return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

} // namespace

TEST_CASE("FileDescriptor is move-only", "[FileDescriptor]")
{
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<FileDescriptor>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<FileDescriptor>);

    STATIC_REQUIRE(std::is_move_constructible_v<FileDescriptor>);
    STATIC_REQUIRE(std::is_move_assignable_v<FileDescriptor>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<FileDescriptor>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<FileDescriptor>);
    STATIC_REQUIRE(std::is_nothrow_destructible_v<FileDescriptor>);
}

TEST_CASE("FileDescriptor conversions are explicit", "[FileDescriptor]")
{
    STATIC_REQUIRE_FALSE(std::is_convertible_v<int, FileDescriptor>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<FileDescriptor, bool>);
}

TEST_CASE("Default-constructed FileDescriptor owns nothing", "[FileDescriptor]")
{
    const FileDescriptor descriptor;

    CHECK(descriptor.get() == -1);
    CHECK_FALSE(descriptor.valid());
    CHECK_FALSE(static_cast<bool>(descriptor));
}

TEST_CASE("FileDescriptor takes ownership of a valid descriptor", "[FileDescriptor]")
{
    const auto [readFd, writeFd] = makePipe();
    const FileDescriptor writeEnd{writeFd};

    const FileDescriptor descriptor{readFd};

    CHECK(descriptor.get() == readFd);
    CHECK(descriptor.valid());
    CHECK(static_cast<bool>(descriptor));
    CHECK(isOpen(readFd));
}

TEST_CASE("FileDescriptor destructor closes the owned descriptor", "[FileDescriptor]")
{
    const auto [readFd, writeFd] = makePipe();
    const FileDescriptor writeEnd{writeFd};

    {
        const FileDescriptor descriptor{readFd};
        REQUIRE(isOpen(readFd));
    }

    CHECK(isClosed(readFd));
}

TEST_CASE("FileDescriptor move construction transfers ownership", "[FileDescriptor]")
{
    const auto [readFd, writeFd] = makePipe();
    const FileDescriptor writeEnd{writeFd};

    std::optional<FileDescriptor> destination;
    {
        FileDescriptor source{readFd};
        destination.emplace(std::move(source));

        CHECK(destination->get() == readFd);
        CHECK(source.get() == -1);
        CHECK_FALSE(source.valid());
    }

    // The moved-from source is gone and must not have closed the descriptor.
    CHECK(isOpen(readFd));

    destination.reset();
    CHECK(isClosed(readFd));
}

TEST_CASE("FileDescriptor move assignment", "[FileDescriptor]")
{
    const auto [sourceFd, destinationFd] = makePipe();

    SECTION("closes the destination's descriptor and takes the source's")
    {
        FileDescriptor destination{destinationFd};
        {
            FileDescriptor source{sourceFd};
            destination = std::move(source);

            CHECK(isClosed(destinationFd));
            CHECK(destination.get() == sourceFd);
            CHECK(source.get() == -1);
            CHECK_FALSE(source.valid());
        }

        // The moved-from source is gone and must not have closed the descriptor.
        CHECK(isOpen(sourceFd));
    }

    SECTION("into an empty destination takes the source's descriptor")
    {
        const FileDescriptor other{destinationFd};
        FileDescriptor destination;
        FileDescriptor source{sourceFd};

        destination = std::move(source);

        CHECK(destination.get() == sourceFd);
        CHECK(source.get() == -1);
        CHECK(isOpen(sourceFd));
    }

    SECTION("from an empty source closes the destination's descriptor")
    {
        const FileDescriptor other{sourceFd};
        FileDescriptor destination{destinationFd};
        FileDescriptor source;

        destination = std::move(source);

        CHECK(isClosed(destinationFd));
        CHECK(destination.get() == -1);
        CHECK(source.get() == -1);
    }
}

TEST_CASE("FileDescriptor self move assignment keeps the descriptor", "[FileDescriptor]")
{
    const auto [readFd, writeFd] = makePipe();
    const FileDescriptor writeEnd{writeFd};

    {
        FileDescriptor descriptor{readFd};

        // The self-move is the point of this test, so silence -Wself-move here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
        descriptor = std::move(descriptor);
#pragma GCC diagnostic pop

        CHECK(descriptor.get() == readFd);
        CHECK(descriptor.valid());
        CHECK(isOpen(readFd));
    }

    CHECK(isClosed(readFd));
}

TEST_CASE("FileDescriptor release gives up ownership without closing", "[FileDescriptor]")
{
    const auto [readFd, writeFd] = makePipe();
    const FileDescriptor writeEnd{writeFd};

    int released = -1;
    {
        FileDescriptor descriptor{readFd};
        released = descriptor.release();

        CHECK(released == readFd);
        CHECK(descriptor.get() == -1);
        CHECK_FALSE(descriptor.valid());
    }

    CHECK(isOpen(readFd));

    // The caller now owns the descriptor and must close it.
    CHECK(::close(released) == 0);
}

TEST_CASE("FileDescriptor reset", "[FileDescriptor]")
{
    const auto [firstFd, secondFd] = makePipe();

    SECTION("without an argument closes the owned descriptor")
    {
        const FileDescriptor other{secondFd};
        FileDescriptor descriptor{firstFd};

        descriptor.reset();

        CHECK(isClosed(firstFd));
        CHECK(descriptor.get() == -1);
        CHECK_FALSE(descriptor.valid());
    }

    SECTION("with a new descriptor closes the old one and owns the new one")
    {
        FileDescriptor descriptor{firstFd};

        descriptor.reset(secondFd);

        CHECK(isClosed(firstFd));
        CHECK(descriptor.get() == secondFd);
        CHECK(descriptor.valid());
        CHECK(isOpen(secondFd));
    }

    SECTION("with the currently owned descriptor keeps it open")
    {
        const FileDescriptor other{secondFd};
        FileDescriptor descriptor{firstFd};

        descriptor.reset(descriptor.get());

        CHECK(descriptor.get() == firstFd);
        CHECK(descriptor.valid());
        CHECK(isOpen(firstFd));
    }

    SECTION("on an empty descriptor")
    {
        const FileDescriptor other{secondFd};
        FileDescriptor descriptor;

        descriptor.reset();
        CHECK(descriptor.get() == -1);

        descriptor.reset(firstFd);
        CHECK(descriptor.get() == firstFd);
        CHECK(isOpen(firstFd));
    }
}

TEST_CASE("FileDescriptor cleanup preserves errno", "[FileDescriptor]")
{
    const auto [readFd, writeFd] = makePipe();
    const FileDescriptor writeEnd{writeFd};

    {
        const FileDescriptor descriptor{readFd};

        // Close behind the wrapper's back so its own close() fails with EBADF.
        REQUIRE(::close(readFd) == 0);
        errno = ENOENT;
    }
    const int errnoAfterCleanup = errno;

    CHECK(errnoAfterCleanup == ENOENT);
}
