#pragma once
#include "editor/engine_export.h"
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <algorithm>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

// The watchdog breaks a blocked write when ffmpeg is cancelled or the editor closes.
class EncoderPipe {
#ifdef _WIN32
    HANDLE process_ = nullptr, input_ = nullptr;
    std::thread watcher_;
    std::atomic<bool> done_{false};
public:
    EncoderPipe(const std::string& command, EngineExportProgress& progress, const std::filesystem::path& log) {
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
        HANDLE read = nullptr;
        if (!CreatePipe(&read,&input_,&attributes,0)) throw std::runtime_error("Cannot create encoder input pipe");
        SetHandleInformation(input_,HANDLE_FLAG_INHERIT,0);
        HANDLE nullFile = CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&attributes,OPEN_EXISTING,0,nullptr);
        HANDLE logFile = CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&attributes,CREATE_ALWAYS,0,nullptr);
        if (logFile == INVALID_HANDLE_VALUE) { CloseHandle(read); CloseHandle(input_); input_ = nullptr; if (nullFile != INVALID_HANDLE_VALUE) CloseHandle(nullFile); throw std::runtime_error("Cannot open encoder log"); }
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = read; startup.hStdOutput = nullFile; startup.hStdError = logFile;
        PROCESS_INFORMATION child{};
        int count = MultiByteToWideChar(CP_UTF8,0,command.c_str(),-1,nullptr,0);
        std::wstring text(count,L'\0'); MultiByteToWideChar(CP_UTF8,0,command.c_str(),-1,text.data(),count);
        BOOL ok = CreateProcessW(nullptr,text.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child);
        CloseHandle(read); CloseHandle(logFile); if (nullFile != INVALID_HANDLE_VALUE) CloseHandle(nullFile);
        if (!ok) { CloseHandle(input_); input_ = nullptr; throw std::runtime_error("Cannot start ffmpeg; check executable path"); }
        CloseHandle(child.hThread); process_ = child.hProcess;
        try {
            watcher_ = std::thread([this,&progress] {
                while (!done_ && WaitForSingleObject(process_,20) == WAIT_TIMEOUT) {
                    if (progress.gate == EngineExportProgress::Cancelled) { TerminateProcess(process_,1); break; }
                }
            });
        } catch (...) { TerminateProcess(process_,1); CloseHandle(input_); CloseHandle(process_); process_ = input_ = nullptr; throw; }
    }
    bool write(const std::vector<unsigned char>& bytes) {
        size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            if (!WriteFile(input_,bytes.data()+offset,(DWORD)std::min<size_t>(bytes.size()-offset,1u<<30),&written,nullptr) || !written) return false;
            offset += written;
        }
        return true;
    }
    int finish() {
        CloseHandle(input_); input_ = nullptr;
        WaitForSingleObject(process_,INFINITE);
        DWORD code = 1; GetExitCodeProcess(process_,&code);
        done_ = true; watcher_.join(); CloseHandle(process_); process_ = nullptr;
        return (int)code;
    }
    ~EncoderPipe() {
        if (process_) { TerminateProcess(process_,1); done_ = true; if (watcher_.joinable()) watcher_.join(); CloseHandle(process_); }
        if (input_) CloseHandle(input_);
    }
#else
    FILE* pipe_ = nullptr;
public:
    // shortcut: POSIX cancellation is checked between frames; add a process handle before shipping that platform.
    EncoderPipe(const std::string& command, EngineExportProgress&, const std::filesystem::path&) { pipe_ = popen(command.c_str(),"w"); if (!pipe_) throw std::runtime_error("Cannot start ffmpeg"); }
    bool write(const std::vector<unsigned char>& bytes) { return fwrite(bytes.data(),1,bytes.size(),pipe_) == bytes.size(); }
    int finish() { auto* pipe = pipe_; pipe_ = nullptr; return pclose(pipe); }
    ~EncoderPipe() { if (pipe_) pclose(pipe_); }
#endif
    EncoderPipe(const EncoderPipe&) = delete;
    EncoderPipe& operator=(const EncoderPipe&) = delete;
};
