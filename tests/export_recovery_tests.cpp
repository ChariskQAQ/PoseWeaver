#include "editor/export_transaction.h"
#include <json.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace fs=std::filesystem;
using nlohmann::json;
static int failures=0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n",__LINE__,#x); ++failures; } } while(false)
static void put(const fs::path& p,const std::string& s) { std::ofstream(p,std::ios::binary)<<s; }
static std::string get(const fs::path& p) { std::ifstream in(p,std::ios::binary); return std::string(std::istreambuf_iterator<char>(in),{}); }
struct ExportTransactionTestAccess {
    static void prepare(ExportTransaction& t) { t.prepare(); }
    static void done(ExportTransaction& t) { t.journal(true); }
    static void retain(ExportTransaction& t) { t.preserve_=true; }
};
static void crash(const fs::path& root,int phase) {
    put(root/"first.png","old first"); put(root/"tail.png","old tail");
    ExportTransaction t(root,root/"registry");
    put(t.directory/"first.png","new first"); put(t.directory/"new.png","new file");
    t.add(t.directory/"first.png",root/"first.png"); t.add(t.directory/"new.png",root/"new.png"); t.remove(root/"tail.png");
    ExportTransactionTestAccess::prepare(t);
    if (phase>=1) fs::rename(root/"first.png",t.directory/"backup-0");
    if (phase>=2) fs::rename(t.directory/"first.png",root/"first.png");
    if (phase>=3) fs::rename(t.directory/"new.png",root/"new.png");
    if (phase>=4) fs::rename(root/"tail.png",t.directory/"backup-2");
    if (phase==5) ExportTransactionTestAccess::done(t);
    if (phase==6) { // Interrupt recovery after one replacement has already been restored.
        fs::rename(root/"first.png",t.directory/"first.png"); fs::rename(t.directory/"backup-0",root/"first.png");
    }
    std::_Exit(42);
}
static bool child(const fs::path& root,int phase) {
#ifdef _WIN32
    wchar_t executable[32768]; GetModuleFileNameW(nullptr,executable,32768);
    auto command=L"\""+std::wstring(executable)+L"\" --crash \""+root.wstring()+L"\" "+std::to_wstring(phase);
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)) return false;
    bool ended=WaitForSingleObject(process.hProcess,15000)==WAIT_OBJECT_0;
    if (!ended) TerminateProcess(process.hProcess,99);
    DWORD code=0; GetExitCodeProcess(process.hProcess,&code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return ended && code==42;
#else
    auto pid=fork(); if (pid==0) crash(root,phase); if (pid<0) return false;
    int code; waitpid(pid,&code,0); return WIFEXITED(code) && WEXITSTATUS(code)==42;
#endif
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    if (argc==4 && std::wstring(argv[1])==L"--crash") crash(fs::path(argv[2]),std::stoi(argv[3]));
#else
int main() {
#endif
    const auto parent=fs::current_path(), root=parent/("export-recovery-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    for (int phase=0;phase<=6;++phase) {
        auto dir=root/std::to_string(phase); fs::create_directory(dir); CHECK(child(dir,phase));
        auto records=ExportTransaction::pending(dir/"registry"); CHECK(records.size()==1);
        if (records.size()!=1) continue;
        CHECK(records[0].error.empty() && records[0].committed==(phase==5));
        std::string error; bool ok=ExportTransaction::recover(records[0],error);
        if (!ok) std::printf("Recovery error: %s\n",error.c_str()); CHECK(ok);
        CHECK(ExportTransaction::pending(dir/"registry").empty());
        CHECK(get(dir/"first.png")== (phase==5 ? "new first" : "old first"));
        CHECK(fs::exists(dir/"new.png")== (phase==5)); CHECK(fs::exists(dir/"tail.png")== (phase!=5));
    }
    {
        auto dir=root/"conflict"; fs::create_directory(dir); CHECK(child(dir,4));
        auto records=ExportTransaction::pending(dir/"registry"); CHECK(records.size()==1);
        if (!records.empty()) {
            put(dir/"first.png","user edit"); std::string error;
            CHECK(!ExportTransaction::recover(records[0],error));
            CHECK(get(dir/"first.png")=="user edit" && get(dir/"new.png")=="new file" && !fs::exists(dir/"tail.png"));
            ExportTransaction blocked(dir,dir/"registry"); put(blocked.directory/"output.png","another export"); blocked.add(blocked.directory/"output.png",dir/"another.png");
            bool refused=false; try { blocked.commit(); } catch (...) { refused=true; } CHECK(refused && !fs::exists(dir/"another.png"));
            put(dir/"first.png","new first"); CHECK(ExportTransaction::recover(records[0],error));
        }
    }
    {
        auto dir=root/"tamper"; fs::create_directory(dir); CHECK(child(dir,4));
        auto records=ExportTransaction::pending(dir/"registry"); CHECK(records.size()==1);
        if (!records.empty()) {
            auto journal=fs::u8path(records[0].directory)/"transaction.json"; auto j=json::parse(get(journal));
            j["files"][0]["target"]="../outside.txt"; put(journal,j.dump()); put(root/"outside.txt","untouched");
            std::string error; CHECK(!ExportTransaction::recover(records[0],error));
            CHECK(get(root/"outside.txt")=="untouched" && get(dir/"first.png")=="new first" && get(dir/"new.png")=="new file");
        }
    }
    {
        auto dir=root/"active"; fs::create_directory(dir);
        ExportTransaction active(dir,dir/"registry"); put(active.directory/"output.png","new"); active.add(active.directory/"output.png",dir/"output.png");
        ExportTransactionTestAccess::prepare(active); CHECK(ExportTransaction::pending(dir/"registry").empty());
    }
    {
        auto dir=root/"unavailable"; fs::create_directory(dir); CHECK(child(dir,4));
        auto records=ExportTransaction::pending(dir/"registry"); CHECK(records.size()==1);
        if (!records.empty()) {
            auto stage=fs::u8path(records[0].directory), disconnected=dir/"temporarily-unmounted";
            fs::rename(stage,disconnected);
            auto unavailable=ExportTransaction::pending(dir/"registry");
            CHECK(unavailable.size()==1 && !unavailable[0].error.empty() && fs::exists(fs::u8path(records[0].entry)));
            fs::rename(disconnected,stage); std::string error; CHECK(ExportTransaction::recover(records[0],error));
        }
    }
#ifdef _WIN32
    {
        auto dir=root/"rollback"; fs::create_directory(dir); put(dir/"first.png","old first"); put(dir/"second.png","old second");
        ExportTransaction t(dir,dir/"registry"); put(t.directory/"first.png","new first"); put(t.directory/"second.png","new second");
        t.add(t.directory/"first.png",dir/"first.png"); t.add(t.directory/"second.png",dir/"second.png");
        auto handle=CreateFileW((dir/"second.png").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        CHECK(handle!=INVALID_HANDLE_VALUE);
        bool failed=false; try { t.commit(); } catch (...) { failed=true; }
        CHECK(failed && get(dir/"first.png")=="old first" && get(dir/"second.png")=="old second");
        if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
#endif
    if (root.parent_path()==parent) fs::remove_all(root);
    std::printf("export recovery: %d failure(s)\n",failures); return failures ? 1 : 0;
}
