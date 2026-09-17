#include "kvstore/db.h"
#include "memtable.h"
#include "wal.h"
#include "sstable.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <shared_mutex>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace kvstore {

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
    }

    Status Load() {
        for (const auto& entry : std::filesystem::directory_iterator(dbname_)) {
            if (entry.path().extension() == ".sst") {
                SSTableReader* reader = new SSTableReader(entry.path().string());
                if (reader->Open().ok()) {
                    sstables_.push_back(reader);
                    std::string stem = entry.path().stem().string();
                    int num = std::stoi(stem);
                    if (num >= next_file_number_) {
                        next_file_number_ = num + 1;
                    }
                } else {
                    delete reader;
                }
            }
        }
        
        std::sort(sstables_.begin(), sstables_.end(), [](SSTableReader* a, SSTableReader* b) {
            return a->filename() > b->filename();
        });

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

    void Compact() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (sstables_.size() <= 1) return;

        std::map<std::string, std::pair<std::string, bool>> merged;
        for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
            (*it)->LoadAll(&merged);
        }

        std::string new_filename = NewFileName();
        SSTableWriter writer(new_filename);
        for (const auto& [k, v] : merged) {
            if (!v.second) {
                writer.Add(k, v.first, false);
            }
        }
        writer.Finish();

        for (auto* sst : sstables_) {
            std::filesystem::remove(sst->filename());
            delete sst;
        }
        sstables_.clear();
        
        SSTableReader* reader = new SSTableReader(new_filename);
        reader->Open();
        sstables_.push_back(reader);
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
