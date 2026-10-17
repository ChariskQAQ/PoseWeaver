#include "platform/file_dialog.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Builds the double-NUL-terminated filter string GetOpenFileName expects.
static std::wstring makeFilter(const char* name, const char* spec) {
    std::wstring f = widen(name);
    f.push_back(L'\0');
    f += widen(spec);
    f.push_back(L'\0');
    f += L"All files";
    f.push_back(L'\0');
    f += L"*.*";
    f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}

std::string openFileDialog(const char* filterName, const char* filterSpec) {
    wchar_t buf[MAX_PATH * 4] = L"";
    std::wstring filter = makeFilter(filterName, filterSpec);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? narrow(buf) : std::string();
}

std::string saveFileDialog(const char* filterName, const char* filterSpec, const char* defaultExt) {
    wchar_t buf[MAX_PATH * 4] = L"";
    std::wstring filter = makeFilter(filterName, filterSpec);
    std::wstring ext = widen(defaultExt);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrDefExt = ext.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn) ? narrow(buf) : std::string();
}

#else

std::string openFileDialog(const char*, const char*) { return {}; }
std::string saveFileDialog(const char*, const char*, const char*) { return {}; }

#endif
