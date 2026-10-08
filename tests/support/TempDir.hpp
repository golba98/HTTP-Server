#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace test {

// A fresh directory under the system temp directory, removed with everything
// in it when the object is destroyed.
class TempDir {
public:
    TempDir()
    {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "http-test-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::system_error{errno, std::generic_category(), "mkdtemp"};
        }
        path_ = pattern;
    }

    ~TempDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    // Writes `contents` to `relative`, creating parent directories as needed.
    void write(const std::filesystem::path& relative, std::string_view contents) const
    {
        const std::filesystem::path file = path_ / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out{file, std::ios::binary};
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!out) {
            throw std::runtime_error{"cannot write " + file.string()};
        }
    }

private:
    std::filesystem::path path_;
};

} // namespace test
