#include "sstable.h"
#include "utils/coding.h"
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
    if (index_.empty() || index_.size() % 100 == 0) {
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
    uint32_t index_offset = current_offset_;
    char len_buf[4];

    EncodeFixed32(len_buf, index_.size());
    out_.write(len_buf, 4);

    for (const auto& entry : index_) {
        EncodeFixed32(len_buf, entry.key.size());
        out_.write(len_buf, 4);
        out_.write(entry.key.data(), entry.key.size());

        EncodeFixed32(len_buf, entry.offset);
        out_.write(len_buf, 4);
    }

    EncodeFixed32(len_buf, index_offset);
    out_.write(len_buf, 4);

    out_.close();
    return Status::OK();
}

SSTableReader::SSTableReader(const std::string& filename)
    : filename_(filename) {}

SSTableReader::~SSTableReader() {
    if (in_.is_open()) in_.close();
}

Status SSTableReader::Open() {
    in_.open(filename_, std::ios::binary);
    if (!in_.is_open()) return Status::IOError("Failed to open SSTable: " + filename_);

    in_.seekg(-4, std::ios::end);
    uint32_t file_size = in_.tellg() + std::streampos(4);
    char buf[4];
    in_.read(buf, 4);
    uint32_t index_offset = DecodeFixed32(buf);
    
    data_offset_ = 0;
    data_size_ = index_offset;

    in_.seekg(index_offset, std::ios::beg);
    in_.read(buf, 4);
    uint32_t num_entries = DecodeFixed32(buf);

    for (uint32_t i = 0; i < num_entries; i++) {
        in_.read(buf, 4);
        uint32_t key_len = DecodeFixed32(buf);
        std::string key(key_len, '\0');
        in_.read(&key[0], key_len);

        in_.read(buf, 4);
        uint32_t offset = DecodeFixed32(buf);

        index_.push_back({key, offset});
    }

    return Status::OK();
}

Status SSTableReader::Get(const std::string& key, std::string* value) const {
    uint32_t search_offset = 0;
    uint32_t end_offset = data_size_;
    
    for (size_t i = 0; i < index_.size(); i++) {
        if (index_[i].key <= key) {
            search_offset = index_[i].offset;
            if (i + 1 < index_.size()) {
                end_offset = index_[i+1].offset;
            } else {
                end_offset = data_size_;
            }
        } else {
            break;
        }
    }

    in_.seekg(search_offset, std::ios::beg);
    char buf[4];

    while (in_.tellg() < end_offset && in_.read(buf, 4)) {
        uint32_t klen = DecodeFixed32(buf);
        std::string k(klen, '\0');
        in_.read(&k[0], klen);

        in_.read(buf, 4);
        uint32_t vlen = DecodeFixed32(buf);
        std::string v(vlen, '\0');
        in_.read(&v[0], vlen);

        uint8_t is_tombstone;
        in_.read(reinterpret_cast<char*>(&is_tombstone), 1);

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
    in_.seekg(0, std::ios::beg);
    char buf[4];

    while (in_.tellg() < data_size_ && in_.read(buf, 4)) {
        uint32_t klen = DecodeFixed32(buf);
        std::string k(klen, '\0');
        in_.read(&k[0], klen);

        in_.read(buf, 4);
        uint32_t vlen = DecodeFixed32(buf);
        std::string v(vlen, '\0');
        in_.read(&v[0], vlen);

        uint8_t is_tombstone;
        in_.read(reinterpret_cast<char*>(&is_tombstone), 1);

        (*out)[k] = {v, is_tombstone == 1};
    }
}

} // namespace kvstore
