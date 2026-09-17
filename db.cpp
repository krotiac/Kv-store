#include "db.h"
#include "memtable.h"
#include "wal.h"
#include "sstable.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kvstore {

namespace {

bool AtomicReplace(const std::string& src, const std::string& dst) {
    std::error_code ec;
#if defined(_WIN32)
    if (MoveFileExA(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
#endif
    std::filesystem::remove(dst, ec);
    std::filesystem::rename(src, dst, ec);
    return !ec;
}

} // namespace

class DBImpl : public DB {
public:
    DBImpl(const Options& options, const std::string& dbname)
        : options_(options), dbname_(dbname), memtable_(new MemTable()), next_file_number_(1) {}

    virtual ~DBImpl() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (memtable_->ApproximateSize() > 0) {
            FlushMemTable();
            wal_.Clear();
        }
        delete memtable_;
        for (auto* sst : sstables_) {
            delete sst;
        }
        sstables_.clear();
    }

    Status Load() {
        std::error_code ec;
        // Quarantine / clean up incomplete temporary artifacts from previous interrupted operations
        std::filesystem::remove(dbname_ + "/MANIFEST.tmp", ec);
        std::filesystem::remove(dbname_ + "/compact_tmp.sst", ec);

        std::string manifest_path = dbname_ + "/MANIFEST";
        bool manifest_loaded = false;

        if (std::filesystem::exists(manifest_path)) {
            std::ifstream mfile(manifest_path);
            if (mfile.is_open()) {
                std::string header;
                if (std::getline(mfile, header) && header == "MANIFEST_V1") {
                    std::string sst_relname;
                    while (std::getline(mfile, sst_relname)) {
                        while (!sst_relname.empty() && (sst_relname.back() == '\r' || sst_relname.back() == ' ')) {
                            sst_relname.pop_back();
                        }
                        if (sst_relname.empty()) continue;

                        std::string full_path = dbname_ + "/" + sst_relname;
                        if (std::filesystem::exists(full_path)) {
                            SSTableReader* reader = new SSTableReader(full_path);
                            if (reader->Open().ok()) {
                                sstables_.push_back(reader);
                                std::string stem = std::filesystem::path(sst_relname).stem().string();
                                try {
                                    int num = std::stoi(stem);
                                    if (num >= next_file_number_) {
                                        next_file_number_ = num + 1;
                                    }
                                } catch (...) {}
                            } else {
                                delete reader;
                            }
                        }
                    }
                    manifest_loaded = true;
                }
            }
        }

        if (!manifest_loaded) {
            // Fallback for missing or corrupted manifest: discover existing .sst files
            for (const auto& entry : std::filesystem::directory_iterator(dbname_)) {
                if (entry.path().extension() == ".sst" && entry.path().filename().string() != "compact_tmp.sst") {
                    SSTableReader* reader = new SSTableReader(entry.path().string());
                    if (reader->Open().ok()) {
                        sstables_.push_back(reader);
                        std::string stem = entry.path().stem().string();
                        try {
                            int num = std::stoi(stem);
                            if (num >= next_file_number_) {
                                next_file_number_ = num + 1;
                            }
                        } catch (...) {}
                    } else {
                        delete reader;
                    }
                }
            }
            std::sort(sstables_.begin(), sstables_.end(), [](SSTableReader* a, SSTableReader* b) {
                return a->filename() > b->filename();
            });

            // Initialize manifest with discovered tables
            std::vector<std::string> active;
            for (auto* sst : sstables_) {
                active.push_back(std::filesystem::path(sst->filename()).filename().string());
            }
            WriteManifest(active);
        }

        // Replay WAL
        std::string wal_path = dbname_ + "/wal.log";
        WAL::Replay(wal_path, memtable_);

        return wal_.OpenForAppend(wal_path);
    }

    Status Put(const std::string& key, const std::string& value) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        Status s = wal_.AppendPut(key, value);
        if (!s.ok()) return s;

        memtable_->Put(key, value);
        
        if (memtable_->ApproximateSize() >= options_.write_buffer_size) {
            FlushMemTable();
            wal_.Clear();
            wal_.OpenForAppend(dbname_ + "/wal.log");
        }
        return Status::OK();
    }

    Status Delete(const std::string& key) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        Status s = wal_.AppendDelete(key);
        if (!s.ok()) return s;

        memtable_->Delete(key);

        if (memtable_->ApproximateSize() >= options_.write_buffer_size) {
            FlushMemTable();
            wal_.Clear();
            wal_.OpenForAppend(dbname_ + "/wal.log");
        }
        return Status::OK();
    }

    Status Get(const std::string& key, std::string* value) override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        Status s = memtable_->Get(key, value);
        if (s.ok()) return s;
        if (s.IsNotFound() && s.ToString() == "NotFound: Tombstone") return Status::NotFound("Key not found");

        for (auto* sst : sstables_) {
            s = sst->Get(key, value);
            if (s.ok()) return s;
            if (s.IsNotFound() && s.ToString() == "NotFound: Tombstone") return Status::NotFound("Key not found");
        }
        return Status::NotFound("Key not found");
    }

    void Compact() override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (sstables_.size() <= 1) return;

        // 1. Read / merge existing SSTables (oldest to newest)
        std::map<std::string, std::pair<std::string, bool>> merged;
        for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
            (*it)->LoadAll(&merged);
        }

        // 2. Write new compacted SSTable to a temporary file
        std::string temp_sst = dbname_ + "/compact_tmp.sst";
        std::error_code ec;
        std::filesystem::remove(temp_sst, ec);

        SSTableWriter writer(temp_sst);
        for (const auto& [k, v] : merged) {
            if (!v.second) { // Omit purged tombstones
                writer.Add(k, v.first, false);
            }
        }

        // 3. Ensure the new SSTable is successfully completed
        Status s = writer.Finish();
        if (!s.ok()) {
            std::filesystem::remove(temp_sst, ec);
            return;
        }

        std::string new_sst = NewFileName();
        if (!AtomicReplace(temp_sst, new_sst)) {
            std::filesystem::remove(temp_sst, ec);
            return;
        }

        // 4. Atomically replace/update the MANIFEST so the new SSTable becomes active
        std::vector<std::string> new_active_files = {
            std::filesystem::path(new_sst).filename().string()
        };
        if (!WriteManifest(new_active_files)) {
            std::filesystem::remove(new_sst, ec);
            return;
        }

        // 5. Only AFTER manifest update succeeds, remove obsolete SSTables
        std::vector<std::string> old_files_to_delete;
        for (auto* sst : sstables_) {
            old_files_to_delete.push_back(sst->filename());
            delete sst;
        }
        sstables_.clear();

        for (const auto& file_path : old_files_to_delete) {
            std::filesystem::remove(file_path, ec);
        }

        // 6. Reopen/load the active SSTable set
        SSTableReader* reader = new SSTableReader(new_sst);
        if (reader->Open().ok()) {
            sstables_.push_back(reader);
        } else {
            delete reader;
        }
    }

private:
    Options options_;
    std::string dbname_;
    MemTable* memtable_;
    WAL wal_;
    std::vector<SSTableReader*> sstables_;
    int next_file_number_;
    mutable std::shared_mutex mutex_;

    std::string NewFileName() {
        std::ostringstream ss;
        ss << dbname_ << "/" << std::setw(6) << std::setfill('0') << next_file_number_++ << ".sst";
        return ss.str();
    }

    bool WriteManifest(const std::vector<std::string>& active_sstables) {
        std::string tmp_manifest = dbname_ + "/MANIFEST.tmp";
        std::string real_manifest = dbname_ + "/MANIFEST";

        {
            std::ofstream out(tmp_manifest, std::ios::trunc);
            if (!out.is_open()) return false;
            out << "MANIFEST_V1\n";
            for (const auto& f : active_sstables) {
                out << f << "\n";
            }
            out.flush();
            if (!out.good()) return false;
        }

        return AtomicReplace(tmp_manifest, real_manifest);
    }

    void FlushMemTable() {
        std::string new_filename = NewFileName();
        SSTableWriter writer(new_filename);
        
        MemTable::Iterator* it = memtable_->NewIterator();
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            writer.Add(it->key(), it->value(), it->IsTombstone());
        }
        writer.Finish();
        delete it;
        
        SSTableReader* reader = new SSTableReader(new_filename);
        if (reader->Open().ok()) {
            sstables_.insert(sstables_.begin(), reader);
            // Record new SSTable in manifest
            std::vector<std::string> active;
            for (auto* sst : sstables_) {
                active.push_back(std::filesystem::path(sst->filename()).filename().string());
            }
            WriteManifest(active);
        } else {
            delete reader;
        }

        delete memtable_;
        memtable_ = new MemTable();
    }
};

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
    std::error_code ec;
    std::filesystem::create_directories(name, ec);
    if (ec) {
        return Status::IOError("Failed to create database directory: " + name);
    }

    DBImpl* impl = new DBImpl(options, name);
    Status s = impl->Load();
    if (!s.ok()) {
        delete impl;
        return s;
    }

    *dbptr = impl;
    return Status::OK();
}

} // namespace kvstore
