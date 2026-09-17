#ifndef KVSTORE_MEMTABLE_H_
#define KVSTORE_MEMTABLE_H_

#include "status.h"
#include <string>
#include <vector>
#include <random>
#include <iostream>

namespace kvstore {

class MemTable {
private:
    struct Node {
        std::string key;
        std::string value;
        bool is_tombstone;
        std::vector<Node*> forward;

        Node(const std::string& k, const std::string& v, bool tombstone, int level)
            : key(k), value(v), is_tombstone(tombstone), forward(level, nullptr) {}
    };

public:
    MemTable();
    ~MemTable();

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    void Put(const std::string& key, const std::string& value);
    Status Get(const std::string& key, std::string* value) const;
    void Delete(const std::string& key);
    bool Exists(const std::string& key) const;

    // Phase 2/3 persistence
    void Serialize(std::ostream& out) const;
    bool Deserialize(std::istream& in);

    // Iteration support for SSTable flushing
    class Iterator {
    public:
        Iterator(const MemTable* table);
        void SeekToFirst();
        bool Valid() const;
        void Next();
        std::string key() const;
        std::string value() const;
        bool IsTombstone() const;
    private:
        const MemTable* table_;
        Node* current_;
    };

    Iterator* NewIterator() const { return new Iterator(this); }
    size_t ApproximateSize() const { return size_; }

private:

    int RandomLevel();
    Node* head_;
    int max_level_;
    size_t size_;
    std::mt19937 rnd_;
};

} // namespace kvstore

#endif // KVSTORE_MEMTABLE_H_

