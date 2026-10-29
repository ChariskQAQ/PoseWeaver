#include "platform/file_dialog.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <wrl/client.h>

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

std::string saveFileDialog(const char* filterName, const char* filterSpec, const char* defaultExt,
                           bool* collectResources, const char* collectLabel) {
    if (collectResources) {
        struct Apartment {
            HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
        } apartment;
        using Microsoft::WRL::ComPtr;
        ComPtr<IFileSaveDialog> dialog;
        ComPtr<IFileDialogCustomize> controls;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf()))) &&
            SUCCEEDED(dialog.As(&controls))) {
            const auto name = widen(filterName), spec = widen(filterSpec), ext = widen(defaultExt);
            const auto label = widen(collectLabel ? collectLabel : "Collect project resources (images and audio)");
            const COMDLG_FILTERSPEC filters[] = {{name.c_str(), spec.c_str()}, {L"All files", L"*.*"}};
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
            dialog->SetFileTypes(2, filters);
            dialog->SetDefaultExtension(ext.c_str());
            if (SUCCEEDED(controls->AddCheckButton(1, label.c_str(), *collectResources))) {
                if (FAILED(dialog->Show(GetActiveWindow()))) return {};
                ComPtr<IShellItem> item;
                PWSTR file = nullptr;
                if (FAILED(dialog->GetResult(item.GetAddressOf())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &file))) return {};
                std::string path = narrow(file);
                CoTaskMemFree(file);
                BOOL checked = *collectResources;
                if (SUCCEEDED(controls->GetCheckButtonState(1, &checked))) *collectResources = checked != FALSE;
                return path;
            }
        }
        // The file-menu checkbox remains available if the modern system dialog is unavailable.
    }
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

std::string openFolderDialog(const char* title) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.GetAddressOf())))) return {};
    FILEOPENDIALOGOPTIONS options = 0; dialog->GetOptions(&options);
    dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
    const auto label = widen(title); dialog->SetTitle(label.c_str());
    if (FAILED(dialog->Show(GetActiveWindow()))) return {};
    ComPtr<IShellItem> item; PWSTR folder = nullptr;
    if (FAILED(dialog->GetResult(item.GetAddressOf())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&folder))) return {};
    std::string path = narrow(folder); CoTaskMemFree(folder); return path;
}

#else

std::string openFileDialog(const char*, const char*) { return {}; }
std::string openFolderDialog(const char*) { return {}; }
std::string saveFileDialog(const char*, const char*, const char*, bool*, const char*) { return {}; }

#endif
