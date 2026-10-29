#include "editor/export_transaction.h"
#include "core/file_io.h"
#include <json.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <set>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace fs = std::filesystem;
using nlohmann::json;
namespace {
bool regular(const fs::path& p) { return fs::is_regular_file(fs::symlink_status(p)); }
bool present(const fs::path& p) { return fs::exists(fs::symlink_status(p)); }
std::string fingerprint(const fs::path& p) {
    if (!regular(p)) throw std::runtime_error("Recovery file is not a regular file: "+p.u8string());
    std::ifstream in(p,std::ios::binary); uint64_t hash=14695981039346656037ull; char bytes[65536];
    if (!in) throw std::runtime_error("Cannot read recovery file");
    while (in) { in.read(bytes,sizeof(bytes)); for (std::streamsize i=0;i<in.gcount();++i) { hash^=(unsigned char)bytes[i]; hash*=1099511628211ull; } }
    if (!in.eof()) throw std::runtime_error("Cannot fingerprint recovery file");
    return std::to_string(fs::file_size(p))+":"+std::to_string(hash);
}
void write(const fs::path& p,const json& j) {
    std::string error; if (!rig::writeFileAtomic(p.u8string(),j.dump(),&error)) throw std::runtime_error(error);
}
json read(const fs::path& p) {
    if (!regular(p) || fs::file_size(p)>8*1024*1024) throw std::runtime_error("Invalid export recovery record");
    std::ifstream in(p,std::ios::binary); return json::parse(in);
}
bool filename(const std::string& s) {
#ifdef _WIN32
    if (s.empty() || s.back()=='.' || s.back()==' ' || s.find_first_of("\"<>|?*")!=std::string::npos) return false;
#endif
    return !s.empty() && s!="." && s!=".." && s.find_first_of("/\\:")==std::string::npos && s.find('\0')==std::string::npos && fs::u8path(s).filename()==fs::u8path(s);
}
std::string key(std::string s) {
#ifdef _WIN32
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c) { return (char)std::tolower(c); });
#endif
    return s;
}
void move(const fs::path& from,const fs::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH)) throw std::system_error((int)GetLastError(),std::system_category(),"Cannot move export file");
#else
    fs::rename(from,to);
#endif
}
void flush(const fs::path& file) {
#ifdef _WIN32
    auto handle=CreateFileW(file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open staged output for flushing");
    bool ok=FlushFileBuffers(handle)!=0; CloseHandle(handle);
#else
    int handle=::open(file.c_str(),O_RDONLY); if (handle<0) throw std::runtime_error("Cannot open staged output for flushing");
    bool ok=::fsync(handle)==0; ::close(handle);
#endif
    if (!ok) throw std::runtime_error("Cannot flush staged output");
}
json loadJournal(const fs::path& directory) {
    if (!fs::is_directory(fs::symlink_status(directory)) || directory.filename().u8string().rfind(".poseweaver-export-",0)!=0) throw std::runtime_error("Invalid recovery directory");
    auto j=read(directory/"transaction.json");
    if (j.at("format")!="poseweaver.export-transaction" || j.at("version")!=1 || j.at("id")!=directory.filename().u8string() ||
        !j.at("committed").is_boolean() || !j.at("files").is_array() || j["files"].empty() || j["files"].size()>10002) throw std::runtime_error("Invalid export journal");
    std::set<std::string> targets,sources; size_t index=0;
    for (const auto& f : j["files"]) {
        auto target=f.at("target").get<std::string>(), source=f.at("source").get<std::string>();
        if (!filename(target) || (!source.empty() && (!filename(source) || key(source)=="transaction.json" || key(source)=="lock" || key(source).rfind("backup-",0)==0 || !sources.insert(key(source)).second)) ||
            f.at("backup")!="backup-"+std::to_string(index++) || !targets.insert(key(target)).second || !f.at("existed").is_boolean() ||
            !f.at("before").is_string() || !f.at("after").is_string()) throw std::runtime_error("Invalid export journal file");
    }
    return j;
}
void rollback(const fs::path& dir,const json& j) {
    // Validate every file first; user edits must not be overwritten by recovery.
    for (const auto& f : j["files"]) {
        auto target=dir.parent_path()/fs::u8path(f["target"].get<std::string>()), backup=dir/fs::u8path(f["backup"].get<std::string>());
        auto name=f["source"].get<std::string>(); auto source=dir/fs::u8path(name);
        bool hasSource=!name.empty() && present(source), hasBackup=present(backup), hasTarget=present(target);
        if (hasSource && fingerprint(source)!=f["after"]) throw std::runtime_error("Staged export was modified; recovery stopped");
        if (hasBackup && (!f["existed"].get<bool>() || fingerprint(backup)!=f["before"])) throw std::runtime_error("Backup was modified; recovery stopped");
        if (hasBackup) {
            if (hasTarget && (name.empty() || hasSource || fingerprint(target)!=f["after"])) throw std::runtime_error("Output was modified; recovery stopped: "+target.u8string());
        } else if (f["existed"].get<bool>()) {
            if (!hasTarget || fingerprint(target)!=f["before"]) throw std::runtime_error("Previous output unavailable; recovery stopped: "+target.u8string());
        } else if (!hasSource && !name.empty() && hasTarget && fingerprint(target)!=f["after"]) throw std::runtime_error("Output was modified; recovery stopped: "+target.u8string());
    }
    for (auto it=j["files"].rbegin();it!=j["files"].rend();++it) {
        const auto& f=*it;
        auto target=dir.parent_path()/fs::u8path(f["target"].get<std::string>()), backup=dir/fs::u8path(f["backup"].get<std::string>());
        auto name=f["source"].get<std::string>(); auto source=dir/fs::u8path(name);
        if (!name.empty() && !present(source) && present(target) && (present(backup) || !f["existed"].get<bool>())) move(target,source);
        if (present(backup)) move(backup,target);
    }
}
void cleanup(const fs::path& dir,const json& j) {
    std::set<fs::path> allowed{dir/"transaction.json",dir/"lock"};
    for (const auto& f : j["files"]) for (const auto& pair : {std::make_pair("source","after"),std::make_pair("backup","before")}) {
        auto name=f[pair.first].get<std::string>(); if (name.empty()) continue;
        auto file=dir/fs::u8path(name); allowed.insert(file);
        if (present(file) && fingerprint(file)!=f[pair.second]) throw std::runtime_error("Recovery workspace modified; files preserved");
    }
    for (const auto& file : fs::directory_iterator(dir)) if (!allowed.count(file.path()) || !regular(file.path())) throw std::runtime_error("Unexpected recovery workspace contents; files preserved");
    for (const auto& file : allowed) if (file.filename()!="lock" && file.filename()!="transaction.json") fs::remove(file);
}
}
struct ExportTransaction::Lock {
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit Lock(const Path& dir) {
        if (present(dir/"lock") && !regular(dir/"lock")) return;
        handle=CreateFileW((dir/"lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    }
    bool valid() const { return handle!=INVALID_HANDLE_VALUE; }
    ~Lock() { if (valid()) CloseHandle(handle); }
#else
    int handle=-1;
    explicit Lock(const Path& dir) {
        if (present(dir/"lock") && !regular(dir/"lock")) return;
        handle=::open((dir/"lock").c_str(),O_RDWR|O_CREAT,0600);
        if (handle>=0 && flock(handle,LOCK_EX|LOCK_NB)) { ::close(handle); handle=-1; }
    }
    bool valid() const { return handle>=0; }
    ~Lock() { if (valid()) ::close(handle); }
#endif
};
ExportTransaction::ExportTransaction(const Path& parent,const Path& registry) : registry_(registry) {
    static std::atomic<unsigned> sequence{0};
    for (int i=0;i<16;++i) {
        directory=parent/(".poseweaver-export-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++sequence));
        if (!fs::create_directory(directory)) continue;
        lock_=std::make_unique<Lock>(directory);
        if (!lock_->valid()) throw std::runtime_error("Cannot lock export workspace");
        return;
    }
    throw std::runtime_error("Cannot create export workspace");
}
ExportTransaction::~ExportTransaction() {
    lock_.reset();
    if (!preserve_) {
        std::error_code error;
        if (!entry_.empty()) { fs::remove(entry_,error); if (error) return; }
        fs::remove_all(directory,error);
    }
}
void ExportTransaction::add(const Path& source,const Path& target) {
    if (prepared_ || source.parent_path()!=directory || target.parent_path()!=directory.parent_path()) throw std::runtime_error("Invalid export destination");
    files_.push_back({source,target,directory/("backup-"+std::to_string(files_.size()))});
}
void ExportTransaction::remove(const Path& target) {
    if (prepared_ || target.parent_path()!=directory.parent_path()) throw std::runtime_error("Invalid cleanup destination");
    files_.push_back({{},target,directory/("backup-"+std::to_string(files_.size()))});
}
void ExportTransaction::journal(bool committed) {
    json files=json::array();
    for (const auto& f : files_) files.push_back({{"source",f.source.filename().u8string()},{"target",f.target.filename().u8string()},
        {"backup",f.backup.filename().u8string()},{"existed",f.existed},{"before",f.before},{"after",f.after}});
    write(directory/"transaction.json",{{"format","poseweaver.export-transaction"},{"version",1},{"id",directory.filename().u8string()},{"committed",committed},{"files",files}});
}
void ExportTransaction::prepare() {
    if (files_.empty()) throw std::runtime_error("Export produced no files");
    if (!registry_.empty()) for (const auto& r : pending(registry_)) {
        std::error_code error;
        if (!r.directory.empty() && fs::equivalent(fs::u8path(r.directory).parent_path(),directory.parent_path(),error) && (!r.committed || !r.error.empty()))
            throw std::runtime_error("Resolve the interrupted export in this directory before exporting again");
    }
    for (auto& f : files_) {
        f.existed=present(f.target); if (f.existed) f.before=fingerprint(f.target);
        if (!f.source.empty()) { f.after=fingerprint(f.source); flush(f.source); }
    }
    journal(false);
    loadJournal(directory);
    if (!registry_.empty()) {
        fs::create_directories(registry_); entry_=registry_/(directory.filename().u8string()+".json");
        write(entry_,{{"format","poseweaver.export-recovery"},{"version",1},{"directory",fs::absolute(directory).lexically_normal().u8string()}});
    }
    prepared_=true;
}
void ExportTransaction::commit() {
    prepare(); preserve_=true;
    try {
        for (const auto& f : files_) { if (f.existed) move(f.target,f.backup); if (!f.source.empty()) move(f.source,f.target); }
        journal(true);
    } catch (const std::exception& failure) {
        std::string error=failure.what();
        try { auto j=loadJournal(directory); rollback(directory,j); cleanup(directory,j); preserve_=false; }
        catch (const std::exception& recovery) { error+="; recovery preserved at "+directory.u8string()+": "+recovery.what(); }
        throw std::runtime_error(error);
    }
    // Cleanup failure must retain the successful outputs and the recovery pointer.
    try { cleanup(directory,loadJournal(directory)); preserve_=false; } catch (...) {}
}
std::vector<ExportRecovery> ExportTransaction::pending(const Path& registry) {
    std::vector<ExportRecovery> result; if (!fs::exists(registry)) return result;
    for (const auto& entry : fs::directory_iterator(registry)) {
        if (entry.path().extension()!=".json") continue;
        ExportRecovery r; r.entry=entry.path().u8string();
        try {
            auto pointer=read(entry.path());
            if (pointer.at("format")!="poseweaver.export-recovery" || pointer.at("version")!=1) throw std::runtime_error("Invalid recovery pointer");
            auto dir=fs::u8path(pointer.at("directory").get<std::string>());
            if (!dir.is_absolute() || dir.lexically_normal()!=dir || dir.filename().u8string().rfind(".poseweaver-export-",0)!=0 || dir.filename().u8string()+".json"!=entry.path().filename().u8string()) throw std::runtime_error("Invalid recovery location");
            r.directory=dir.u8string();
            if (!fs::exists(dir)) throw std::runtime_error("Export workspace is unavailable; reconnect the drive before retrying. Recovery record preserved.");
            if (!fs::is_directory(fs::symlink_status(dir))) throw std::runtime_error("Invalid recovery directory");
            Lock lock(dir); if (!lock.valid()) continue;
            r.committed=loadJournal(dir)["committed"].get<bool>();
        } catch (const std::exception& e) { r.error=e.what(); }
        result.push_back(std::move(r));
    }
    return result;
}
bool ExportTransaction::recover(const ExportRecovery& r,std::string& error) {
    try {
        const auto entry=fs::u8path(r.entry), dir=fs::u8path(r.directory); auto pointer=read(entry);
        if (!dir.is_absolute() || dir.lexically_normal()!=dir || dir.filename().u8string().rfind(".poseweaver-export-",0)!=0 || pointer.at("format")!="poseweaver.export-recovery" || pointer.at("version")!=1 ||
            pointer.at("directory")!=dir.u8string() || entry.filename().u8string()!=dir.filename().u8string()+".json" ||
            !fs::is_directory(fs::symlink_status(dir))) throw std::runtime_error("Invalid recovery pointer");
        {
            Lock lock(dir); if (!lock.valid()) throw std::runtime_error("Export is active in another process");
            auto j=loadJournal(dir); if (!j["committed"].get<bool>()) rollback(dir,j); cleanup(dir,j);
        }
        fs::remove(entry); fs::remove(dir/"transaction.json"); fs::remove(dir/"lock"); fs::remove(dir); return true;
    } catch (const std::exception& e) { error=e.what(); return false; }
}
