#include "memtable.h"
#include "coding.h"
#include <iostream>

namespace kvstore {

const int kMaxLevel = 12;

MemTable::MemTable() : max_level_(1), size_(0), rnd_(42) {
    head_ = new Node("", "", false, kMaxLevel);
}

MemTable::~MemTable() {
    Node* curr = head_;
    while (curr != nullptr) {
        Node* next = curr->forward[0];
        delete curr;
        curr = next;
    }
}

int MemTable::RandomLevel() {
    int level = 1;
    while (level < kMaxLevel && (rnd_() % 2) == 0) {
        level++;
    }
    return level;
}

void MemTable::Put(const std::string& key, const std::string& value) {
    std::vector<Node*> update(kMaxLevel, nullptr);
    Node* curr = head_;

    for (int i = max_level_ - 1; i >= 0; i--) {
        while (curr->forward[i] != nullptr && curr->forward[i]->key < key) {
            curr = curr->forward[i];
        }
        update[i] = curr;
    }

    curr = curr->forward[0];

    if (curr != nullptr && curr->key == key) {
        size_ -= curr->value.size();
        curr->value = value;
        curr->is_tombstone = false;
        size_ += value.size();
    } else {
        int lvl = RandomLevel();
        if (lvl > max_level_) {
            for (int i = max_level_; i < lvl; i++) {
                update[i] = head_;
            }
            max_level_ = lvl;
        }

        Node* new_node = new Node(key, value, false, lvl);
        for (int i = 0; i < lvl; i++) {
            new_node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = new_node;
        }
        size_ += key.size() + value.size() + 8;
    }
}

void MemTable::Delete(const std::string& key) {
    // Insert a tombstone
    Put(key, "");
    
    Node* curr = head_;
    for (int i = max_level_ - 1; i >= 0; i--) {
        while (curr->forward[i] != nullptr && curr->forward[i]->key < key) {
            curr = curr->forward[i];
        }
    }
    curr = curr->forward[0];
    if (curr != nullptr && curr->key == key) {
        curr->is_tombstone = true;
    }
}

Status MemTable::Get(const std::string& key, std::string* value) const {
    Node* curr = head_;
    for (int i = max_level_ - 1; i >= 0; i--) {
        while (curr->forward[i] != nullptr && curr->forward[i]->key < key) {
            curr = curr->forward[i];
        }
    }
    curr = curr->forward[0];

    if (curr != nullptr && curr->key == key) {
        if (curr->is_tombstone) {
            return Status::NotFound("Tombstone");
        }
        *value = curr->value;
        return Status::OK();
    }
    return Status::NotFound("Not in MemTable");
}

bool MemTable::Exists(const std::string& key) const {
    std::string dummy;
    return Get(key, &dummy).ok();
}

void MemTable::Serialize(std::ostream& out) const {
    char len_buf[4];
    Node* curr = head_->forward[0];
    while (curr != nullptr) {
        uint8_t is_t = curr->is_tombstone ? 1 : 0;
        out.write(reinterpret_cast<const char*>(&is_t), 1);

        EncodeFixed32(len_buf, static_cast<uint32_t>(curr->key.size()));
        out.write(len_buf, 4);
        out.write(curr->key.data(), curr->key.size());

        EncodeFixed32(len_buf, static_cast<uint32_t>(curr->value.size()));
        out.write(len_buf, 4);
        out.write(curr->value.data(), curr->value.size());

        curr = curr->forward[0];
    }
}

bool MemTable::Deserialize(std::istream& in) {
    char len_buf[4];
    uint8_t is_t;

    while (in.read(reinterpret_cast<char*>(&is_t), 1)) {
        if (!in.read(len_buf, 4)) return false;
        uint32_t key_len = DecodeFixed32(len_buf);
        std::string key(key_len, '\0');
        if (key_len > 0 && !in.read(&key[0], key_len)) return false;

        if (!in.read(len_buf, 4)) return false;
        uint32_t val_len = DecodeFixed32(len_buf);
        std::string value(val_len, '\0');
        if (val_len > 0 && !in.read(&value[0], val_len)) return false;

        Put(key, value);
        if (is_t) {
            Delete(key);
        }
    }
    return true;
}

MemTable::Iterator::Iterator(const MemTable* table) : table_(table), current_(nullptr) {}
void MemTable::Iterator::SeekToFirst() { current_ = table_->head_->forward[0]; }
bool MemTable::Iterator::Valid() const { return current_ != nullptr; }
void MemTable::Iterator::Next() { if (current_) current_ = current_->forward[0]; }
std::string MemTable::Iterator::key() const { return current_->key; }
std::string MemTable::Iterator::value() const { return current_->value; }
bool MemTable::Iterator::IsTombstone() const { return current_->is_tombstone; }

} // namespace kvstore

