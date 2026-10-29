#include "core/file_io.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rig {

bool writeFileAtomic(const std::string& path, std::string_view bytes, std::string* error) {
    namespace fs = std::filesystem;
    static std::atomic<unsigned long> sequence{0};
    if (error) error->clear();
    try {
        const fs::path destination = fs::u8path(path);
#ifdef _WIN32
        const unsigned long process = GetCurrentProcessId();
#else
        const unsigned long process = (unsigned long)getpid();
#endif
        fs::path temporary;
#ifdef _WIN32
        HANDLE file = INVALID_HANDLE_VALUE;
#else
        int file = -1;
#endif
        for (int attempt = 0; attempt < 16; ++attempt) {
            temporary = destination;
            temporary += ".tmp-" + std::to_string(process) + "-" + std::to_string(++sequence);
#ifdef _WIN32
            file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
#else
            file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (file >= 0 || errno != EEXIST) break;
#endif
        }
        auto fail = [&](int code) {
            if (error) *error = std::error_code(code, std::system_category()).message();
            return false;
        };
#ifdef _WIN32
        if (file == INVALID_HANDLE_VALUE) return fail((int)GetLastError());
        bool ok = true;
        int code = 0;
        while (ok && !bytes.empty()) {
            DWORD written = 0;
            const DWORD count = (DWORD)std::min<size_t>(bytes.size(), 1u << 30);
            if (!WriteFile(file, bytes.data(), count, &written, nullptr)) { ok = false; code = (int)GetLastError(); }
            else if (written == 0) { ok = false; code = ERROR_WRITE_FAULT; }
            else bytes.remove_prefix(written);
        }
        if (ok && !FlushFileBuffers(file)) { ok = false; code = (int)GetLastError(); }
        if (!CloseHandle(file) && ok) { ok = false; code = (int)GetLastError(); }
        if (ok && !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            ok = false;
            code = (int)GetLastError();
        }
#else
        if (file < 0) return fail(errno);
        bool ok = true;
        int code = 0;
        while (ok && !bytes.empty()) {
            const ssize_t written = ::write(file, bytes.data(), bytes.size());
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { ok = false; code = errno ? errno : EIO; }
            else bytes.remove_prefix((size_t)written);
        }
        if (ok && ::fsync(file) != 0) { ok = false; code = errno; }
        if (::close(file) != 0 && ok) { ok = false; code = errno; }
        if (ok && ::rename(temporary.c_str(), destination.c_str()) != 0) { ok = false; code = errno; }
#endif
        if (!ok) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return fail(code);
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace rig
