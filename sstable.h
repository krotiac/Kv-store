#ifndef KVSTORE_SSTABLE_H_
#define KVSTORE_SSTABLE_H_

#include "memtable.h"
#include "status.h"
#include <string>
#include <vector>
#include <fstream>
#include <map>

namespace kvstore {

class SSTableWriter {
public:
    SSTableWriter(const std::string& filename);
    ~SSTableWriter();

    Status Add(const std::string& key, const std::string& value, bool is_tombstone);
    Status Finish();

private:
    std::string filename_;
    std::ofstream out_;
    
    struct IndexEntry {
        std::string key;
        uint32_t offset;
    };
    std::vector<IndexEntry> index_;
    uint32_t current_offset_;
    uint32_t num_records_ = 0;
};

class SSTableReader {
public:
    SSTableReader(const std::string& filename);
    ~SSTableReader();

    Status Open();
    Status Get(const std::string& key, std::string* value) const;
    void LoadAll(std::map<std::string, std::pair<std::string, bool>>* out) const;

    std::string filename() const { return filename_; }

private:
    std::string filename_;
    
    struct IndexEntry {
        std::string key;
        uint32_t offset;
    };
    std::vector<IndexEntry> index_;
    uint32_t data_offset_ = 0;
    uint32_t data_size_ = 0;
};

} // namespace kvstore

#endif // KVSTORE_SSTABLE_H_

