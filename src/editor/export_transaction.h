#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
struct ExportRecovery { std::string entry,directory,error; bool committed = false; };
// Flush commit intent before touching outputs; retain backups across process interruption.
class ExportTransaction {
    friend struct ExportTransactionTestAccess;
    using Path = std::filesystem::path;
    struct File { Path source,target,backup; bool existed = false; std::string before,after; };
    struct Lock;
    std::unique_ptr<Lock> lock_;
    std::vector<File> files_;
    Path registry_,entry_;
    bool preserve_ = false,prepared_ = false;
    void prepare();
    void journal(bool committed);
public:
    Path directory;
    explicit ExportTransaction(const Path& parent,const Path& registry = {});
    ~ExportTransaction();
    ExportTransaction(const ExportTransaction&) = delete;
    ExportTransaction& operator=(const ExportTransaction&) = delete;
    void add(const Path& source,const Path& target);
    void remove(const Path& target);
    void commit();
    static std::vector<ExportRecovery> pending(const Path& registry);
    static bool recover(const ExportRecovery& recovery,std::string& error);
};
