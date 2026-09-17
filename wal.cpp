#include "wal.h"
#include "coding.h"
#include <filesystem>
#include <iostream>

namespace kvstore {

enum RecordType : uint8_t {
    kPut = 1,
    kDelete = 2
};

WAL::~WAL() {
    if (out_.is_open()) {
        out_.close();
    }
}

Status WAL::OpenForAppend(const std::string& path) {
    path_ = path;
    out_.open(path_, std::ios::binary | std::ios::app);
    if (!out_.is_open()) {
        return Status::IOError("Failed to open WAL for appending: " + path_);
    }
    return Status::OK();
}

Status WAL::AppendPut(const std::string& key, const std::string& value) {
    if (!out_.is_open()) return Status::IOError("WAL not open");

    uint8_t type = kPut;
    out_.write(reinterpret_cast<const char*>(&type), 1);

    char len_buf[4];
    EncodeFixed32(len_buf, static_cast<uint32_t>(key.size()));
    out_.write(len_buf, 4);
    out_.write(key.data(), key.size());

    EncodeFixed32(len_buf, static_cast<uint32_t>(value.size()));
    out_.write(len_buf, 4);
    out_.write(value.data(), value.size());

    out_.flush();
    return out_.good() ? Status::OK() : Status::IOError("WAL write failed");
}

Status WAL::AppendDelete(const std::string& key) {
    if (!out_.is_open()) return Status::IOError("WAL not open");

    uint8_t type = kDelete;
    out_.write(reinterpret_cast<const char*>(&type), 1);

    char len_buf[4];
    EncodeFixed32(len_buf, static_cast<uint32_t>(key.size()));
    out_.write(len_buf, 4);
    out_.write(key.data(), key.size());

    out_.flush();
    return out_.good() ? Status::OK() : Status::IOError("WAL write failed");
}

void WAL::Clear() {
    if (out_.is_open()) {
        out_.close();
    }
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

void WAL::Replay(const std::string& path, MemTable* memtable) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return; // No WAL to replay

    char len_buf[4];
    uint8_t type;

    while (in.read(reinterpret_cast<char*>(&type), 1)) {
        if (type != kPut && type != kDelete) {
            break; // Corrupted record type
        }

        if (!in.read(len_buf, 4)) break;
        uint32_t key_len = DecodeFixed32(len_buf);
        if (key_len > 10 * 1024 * 1024) break; // Corrupted length
        std::string key(key_len, '\0');
        if (key_len > 0 && !in.read(&key[0], key_len)) break;

        if (type == kPut) {
            if (!in.read(len_buf, 4)) break;
            uint32_t val_len = DecodeFixed32(len_buf);
            if (val_len > 100 * 1024 * 1024) break; // Corrupted length
            std::string value(val_len, '\0');
            if (val_len > 0 && !in.read(&value[0], val_len)) break;
            memtable->Put(key, value);
        } else if (type == kDelete) {
            memtable->Delete(key);
        }
    }
}

} // namespace kvstore

