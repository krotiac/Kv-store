#include "sstable.h"
#include "coding.h"
#include <iostream>

namespace kvstore {

SSTableWriter::SSTableWriter(const std::string& filename)
    : filename_(filename), current_offset_(0) {
    out_.open(filename, std::ios::binary);
}

SSTableWriter::~SSTableWriter() {
    if (out_.is_open()) {
        out_.close();
    }
}

Status SSTableWriter::Add(const std::string& key, const std::string& value, bool is_tombstone) {
    if (!out_.is_open()) return Status::IOError("SSTable not open");

    // Phase 6: Add sparse index
    num_records_++;
    if (index_.empty() || num_records_ % 100 == 0) {
        index_.push_back({key, current_offset_});
    }

    char len_buf[4];
    EncodeFixed32(len_buf, key.size());
    out_.write(len_buf, 4);
    out_.write(key.data(), key.size());

    EncodeFixed32(len_buf, value.size());
    out_.write(len_buf, 4);
    out_.write(value.data(), value.size());

    uint8_t t = is_tombstone ? 1 : 0;
    out_.write(reinterpret_cast<const char*>(&t), 1);

    current_offset_ += 4 + key.size() + 4 + value.size() + 1;
    return Status::OK();
}

Status SSTableWriter::Finish() {
    if (!out_.is_open()) return Status::IOError("SSTable not open");

    uint32_t index_offset = current_offset_;
    char len_buf[4];

    EncodeFixed32(len_buf, static_cast<uint32_t>(index_.size()));
    out_.write(len_buf, 4);

    for (const auto& entry : index_) {
        EncodeFixed32(len_buf, static_cast<uint32_t>(entry.key.size()));
        out_.write(len_buf, 4);
        out_.write(entry.key.data(), entry.key.size());

        EncodeFixed32(len_buf, entry.offset);
        out_.write(len_buf, 4);
    }

    EncodeFixed32(len_buf, index_offset);
    out_.write(len_buf, 4);

    out_.flush();
    bool ok = out_.good();
    out_.close();
    return ok ? Status::OK() : Status::IOError("Failed to write SSTable footer");
}

SSTableReader::SSTableReader(const std::string& filename)
    : filename_(filename), data_offset_(0), data_size_(0) {}

SSTableReader::~SSTableReader() = default;

Status SSTableReader::Open() {
    std::ifstream in(filename_, std::ios::binary);
    if (!in.is_open()) return Status::IOError("Failed to open SSTable: " + filename_);

    in.seekg(0, std::ios::end);
    auto file_size = in.tellg();
    if (file_size < 4) {
        return Status::IOError("SSTable file too small: " + filename_);
    }

    in.seekg(-4, std::ios::end);
    char buf[4];
    if (!in.read(buf, 4)) {
        return Status::IOError("Failed to read SSTable footer: " + filename_);
    }
    uint32_t index_offset = DecodeFixed32(buf);
    if (index_offset > static_cast<uint64_t>(file_size) - 4) {
        return Status::IOError("Corrupted index offset in SSTable: " + filename_);
    }

    data_offset_ = 0;
    data_size_ = index_offset;

    in.seekg(index_offset, std::ios::beg);
    if (!in.read(buf, 4)) {
        return Status::IOError("Failed to read SSTable index header: " + filename_);
    }
    uint32_t num_entries = DecodeFixed32(buf);
    if (num_entries > 10000000) {
        return Status::IOError("Corrupted index entry count in SSTable: " + filename_);
    }

    index_.clear();
    for (uint32_t i = 0; i < num_entries; i++) {
        if (!in.read(buf, 4)) return Status::IOError("Truncated index entry in SSTable: " + filename_);
        uint32_t key_len = DecodeFixed32(buf);
        if (key_len > 10 * 1024 * 1024) return Status::IOError("Corrupted index key length in SSTable: " + filename_);
        std::string key(key_len, '\0');
        if (key_len > 0 && !in.read(&key[0], key_len)) {
            return Status::IOError("Truncated index key in SSTable: " + filename_);
        }

        if (!in.read(buf, 4)) return Status::IOError("Truncated index offset in SSTable: " + filename_);
        uint32_t offset = DecodeFixed32(buf);
        if (offset > data_size_) return Status::IOError("Invalid index offset in SSTable: " + filename_);

        index_.push_back({key, offset});
    }

    return Status::OK();
}

Status SSTableReader::Get(const std::string& key, std::string* value) const {
    if (index_.empty() || key < index_[0].key) {
        return Status::NotFound("Not in SSTable");
    }

    uint32_t search_offset = 0;
    uint32_t end_offset = data_size_;

    for (size_t i = 0; i < index_.size(); i++) {
        if (index_[i].key <= key) {
            search_offset = index_[i].offset;
            if (i + 1 < index_.size()) {
                end_offset = index_[i + 1].offset;
            } else {
                end_offset = data_size_;
            }
        } else {
            break;
        }
    }

    std::ifstream in(filename_, std::ios::binary);
    if (!in.is_open()) {
        return Status::IOError("Failed to open SSTable for read: " + filename_);
    }

    in.seekg(search_offset, std::ios::beg);
    char buf[4];

    while (static_cast<uint32_t>(in.tellg()) < end_offset && in.read(buf, 4)) {
        uint32_t klen = DecodeFixed32(buf);
        if (klen > 10 * 1024 * 1024) return Status::IOError("Corrupted record key length");
        std::string k(klen, '\0');
        if (klen > 0 && !in.read(&k[0], klen)) break;

        if (!in.read(buf, 4)) break;
        uint32_t vlen = DecodeFixed32(buf);
        if (vlen > 100 * 1024 * 1024) return Status::IOError("Corrupted record value length");
        std::string v(vlen, '\0');
        if (vlen > 0 && !in.read(&v[0], vlen)) break;

        uint8_t is_tombstone = 0;
        if (!in.read(reinterpret_cast<char*>(&is_tombstone), 1)) break;

        if (k == key) {
            if (is_tombstone) {
                return Status::NotFound("Tombstone");
            }
            *value = v;
            return Status::OK();
        } else if (k > key) {
            break;
        }
    }
    return Status::NotFound("Not in SSTable");
}

void SSTableReader::LoadAll(std::map<std::string, std::pair<std::string, bool>>* out) const {
    std::ifstream in(filename_, std::ios::binary);
    if (!in.is_open()) return;

    in.seekg(0, std::ios::beg);
    char buf[4];

    while (static_cast<uint32_t>(in.tellg()) < data_size_ && in.read(buf, 4)) {
        uint32_t klen = DecodeFixed32(buf);
        if (klen > 10 * 1024 * 1024) break;
        std::string k(klen, '\0');
        if (klen > 0 && !in.read(&k[0], klen)) break;

        if (!in.read(buf, 4)) break;
        uint32_t vlen = DecodeFixed32(buf);
        if (vlen > 100 * 1024 * 1024) break;
        std::string v(vlen, '\0');
        if (vlen > 0 && !in.read(&v[0], vlen)) break;

        uint8_t is_tombstone = 0;
        if (!in.read(reinterpret_cast<char*>(&is_tombstone), 1)) break;

        (*out)[k] = {v, is_tombstone == 1};
    }
}

} // namespace kvstore

