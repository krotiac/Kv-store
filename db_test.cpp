#include "db.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

using namespace kvstore;

class DBTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean up before each test so tests don't interfere with each other
        std::error_code ec;
        std::filesystem::remove_all("test_db", ec);

        Options options;
        options.create_if_missing = true;
        Status s = DB::Open(options, "test_db", &db_);
        ASSERT_TRUE(s.ok());
        ASSERT_NE(db_, nullptr);
    }

    void TearDown() override {
        delete db_;
        db_ = nullptr;
    }

    DB* db_ = nullptr;
};

TEST_F(DBTest, PutAndGet) {
    Status s = db_->Put("key1", "value1");
    ASSERT_TRUE(s.ok());

    std::string value;
    s = db_->Get("key1", &value);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(value, "value1");
}

TEST_F(DBTest, GetNotFound) {
    std::string value;
    Status s = db_->Get("missing_key", &value);
    ASSERT_TRUE(s.IsNotFound());
    EXPECT_EQ(s.ToString(), "NotFound: Key not found");
}

TEST_F(DBTest, DeleteKey) {
    db_->Put("key1", "value1");
    
    Status s = db_->Delete("key1");
    ASSERT_TRUE(s.ok());

    std::string value;
    s = db_->Get("key1", &value);
    ASSERT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, UpdateKey) {
    db_->Put("key1", "value1");
    db_->Put("key1", "value2");

    std::string value;
    Status s = db_->Get("key1", &value);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(value, "value2");
}

TEST_F(DBTest, PersistenceAcrossRestarts) {
    // Put data
    db_->Put("persisted_key1", "persisted_value1");
    db_->Put("persisted_key2", "persisted_value2");
    
    // Ensure deletes are persisted too (by not being written to the file)
    db_->Delete("persisted_key2");

    // Close DB
    delete db_;
    db_ = nullptr;

    // Reopen DB
    Options options;
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());
    ASSERT_NE(db_, nullptr);

    // Verify data
    std::string value;
    s = db_->Get("persisted_key1", &value);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(value, "persisted_value1");

    s = db_->Get("persisted_key2", &value);
    ASSERT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, EmptyDatabasePersistence) {
    // Close DB without putting anything
    delete db_;
    db_ = nullptr;

    // Reopen DB
    Options options;
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());
    ASSERT_NE(db_, nullptr);

    // Should still be empty
    std::string value;
    s = db_->Get("any_key", &value);
    ASSERT_TRUE(s.IsNotFound());
}

#include "coding.h"

TEST_F(DBTest, WALRecovery) {
    // We simulate a crash by manually writing a WAL file before opening the DB
    // Since there's no data.bin, it should recover entirely from the WAL.
    
    // First, close the current db_ and clean up test_db
    delete db_;
    db_ = nullptr;
    std::error_code ec;
    std::filesystem::remove_all("test_db", ec);
    std::filesystem::create_directories("test_db", ec);

    // Manually write a WAL file
    std::ofstream out("test_db/wal.log", std::ios::binary);
    uint8_t type = 1; // kPut
    out.write(reinterpret_cast<const char*>(&type), 1);
    
    char len_buf[4];
    std::string key = "wal_key";
    std::string val = "wal_val";
    EncodeFixed32(len_buf, key.size());
    out.write(len_buf, 4);
    out.write(key.data(), key.size());
    
    EncodeFixed32(len_buf, val.size());
    out.write(len_buf, 4);
    out.write(val.data(), val.size());
    out.close();

    // Now open the DB
    Options options;
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    // Verify recovery
    std::string value;
    s = db_->Get("wal_key", &value);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(value, "wal_val");
}

TEST_F(DBTest, TruncatedWAL) {
    delete db_;
    db_ = nullptr;
    std::error_code ec;
    std::filesystem::remove_all("test_db", ec);
    std::filesystem::create_directories("test_db", ec);

    // Write a truncated WAL (e.g., stops halfway through value length)
    std::ofstream out("test_db/wal.log", std::ios::binary);
    uint8_t type = 1; // kPut
    out.write(reinterpret_cast<const char*>(&type), 1);
    
    char len_buf[4];
    std::string key = "trunc_key";
    EncodeFixed32(len_buf, key.size());
    out.write(len_buf, 4);
    out.write(key.data(), key.size());
    // Stop here, making it truncated.
    out.close();

    Options options;
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    // Should open successfully but NOT contain trunc_key
    std::string value;
    s = db_->Get("trunc_key", &value);
    ASSERT_TRUE(s.IsNotFound());
}

#include "memtable.h"

TEST(MemTableTest, SkipListOperations) {
    MemTable table;
    table.Put("key3", "val3");
    table.Put("key1", "val1");
    table.Put("key2", "val2");
    table.Put("key5", "val5");

    std::string val;
    ASSERT_TRUE(table.Get("key2", &val).ok());
    EXPECT_EQ(val, "val2");

    ASSERT_TRUE(table.Get("key4", &val).IsNotFound());

    table.Delete("key3");
    Status s = table.Get("key3", &val);
    ASSERT_TRUE(s.IsNotFound());
    EXPECT_EQ(s.ToString(), "NotFound: Tombstone");

    // Sorted Iteration
    MemTable::Iterator* it = table.NewIterator();
    it->SeekToFirst();
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), "key1");
    it->Next();
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), "key2");
    it->Next();
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), "key3");
    EXPECT_TRUE(it->IsTombstone());
    it->Next();
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), "key5");
    it->Next();
    ASSERT_FALSE(it->Valid());
    delete it;
}

TEST(MemTableTest, ApproximateSizeAccounting) {
    MemTable table;
    EXPECT_EQ(table.ApproximateSize(), 0u);

    table.Put("key1", "alpha");
    size_t size1 = table.ApproximateSize();
    EXPECT_GT(size1, 0u);

    // Updating with same length should not grow ApproximateSize
    table.Put("key1", "bravo");
    EXPECT_EQ(table.ApproximateSize(), size1);

    // Repeated updates with same length should keep size constant
    for (int i = 0; i < 50; ++i) {
        table.Put("key1", "val_" + std::to_string(i % 10)); // length 5
    }
    EXPECT_EQ(table.ApproximateSize(), size1);

    // Updating with longer value should increase size by exact difference
    table.Put("key1", "longer_value"); // length 12
    EXPECT_EQ(table.ApproximateSize(), size1 + (12 - 5));

    // Deleting replaces value with empty string ("")
    table.Delete("key1");
    EXPECT_EQ(table.ApproximateSize(), size1 + (12 - 5) - 12);
}

TEST_F(DBTest, SSTablesAndCompaction) {
    Options options;
    options.write_buffer_size = 100; // Small size to force flushes
    delete db_;
    std::filesystem::remove_all("test_db");
    
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    // Write enough data to force multiple SSTables
    for (int i = 0; i < 100; ++i) {
        db_->Put("key" + std::to_string(i), "value" + std::to_string(i));
    }

    // Verify data
    std::string val;
    s = db_->Get("key50", &val);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(val, "value50");

    // Overwrite data
    db_->Put("key50", "new_value50");
    s = db_->Get("key50", &val);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(val, "new_value50");

    // Delete data
    db_->Delete("key51");
    s = db_->Get("key51", &val);
    ASSERT_TRUE(s.IsNotFound());

    // Compact
    db_->Compact();

    // Verify data still correct after compaction
    s = db_->Get("key50", &val);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(val, "new_value50");

    s = db_->Get("key51", &val);
    ASSERT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, ConcurrentSSTableReads) {
    Options options;
    options.write_buffer_size = 50;
    delete db_;
    std::filesystem::remove_all("test_db");
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    for (int i = 0; i < 50; ++i) {
        db_->Put("skey_" + std::to_string(i), "sval_" + std::to_string(i));
    }

    const int num_threads = 8;
    const int reads_per_thread = 200;
    std::atomic<bool> success{true};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, &success, reads_per_thread]() {
            for (int r = 0; r < reads_per_thread; ++r) {
                int key_idx = (r * 7) % 50;
                std::string val;
                Status st = db_->Get("skey_" + std::to_string(key_idx), &val);
                if (!st.ok() || val != "sval_" + std::to_string(key_idx)) {
                    success = false;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }
    EXPECT_TRUE(success.load());
}

TEST_F(DBTest, ConcurrentReaders) {
    for (int i = 0; i < 100; ++i) {
        db_->Put("ckey_" + std::to_string(i), "cval_" + std::to_string(i));
    }

    const int num_threads = 8;
    const int reads_per_thread = 300;
    std::atomic<bool> success{true};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, &success, reads_per_thread]() {
            for (int r = 0; r < reads_per_thread; ++r) {
                int key_idx = r % 100;
                std::string val;
                Status st = db_->Get("ckey_" + std::to_string(key_idx), &val);
                if (!st.ok() || val != "cval_" + std::to_string(key_idx)) {
                    success = false;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }
    EXPECT_TRUE(success.load());
}

TEST_F(DBTest, ConcurrentWriters) {
    const int num_threads = 6;
    const int writes_per_thread = 50;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, writes_per_thread]() {
            for (int i = 0; i < writes_per_thread; ++i) {
                int key_id = t * 1000 + i;
                db_->Put("wkey_" + std::to_string(key_id), "wval_" + std::to_string(key_id));
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    delete db_;
    db_ = nullptr;

    Options options;
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    for (int t = 0; t < num_threads; ++t) {
        for (int i = 0; i < writes_per_thread; ++i) {
            int key_id = t * 1000 + i;
            std::string val;
            Status st = db_->Get("wkey_" + std::to_string(key_id), &val);
            ASSERT_TRUE(st.ok()) << "Missing key: " << key_id;
            EXPECT_EQ(val, "wval_" + std::to_string(key_id));
        }
    }
}

TEST_F(DBTest, ConcurrentMixedReadWrite) {
    for (int i = 0; i < 50; ++i) {
        db_->Put("mkey_" + std::to_string(i), "init_" + std::to_string(i));
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> errors{false};
    std::vector<std::thread> writers;
    std::vector<std::thread> readers;

    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([this, w, &stop]() {
            int step = 0;
            while (!stop.load()) {
                int key_id = (w * 13 + step) % 50;
                db_->Put("mkey_" + std::to_string(key_id), "val_w" + std::to_string(w) + "_" + std::to_string(step));
                step++;
                if (step >= 100) break;
            }
        });
    }

    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([this, &stop, &errors]() {
            int count = 0;
            while (!stop.load() && count < 200) {
                int key_id = count % 50;
                std::string val;
                Status st = db_->Get("mkey_" + std::to_string(key_id), &val);
                if (st.ok()) {
                    if (val.rfind("init_", 0) != 0 && val.rfind("val_w", 0) != 0) {
                        errors = true;
                    }
                }
                count++;
            }
        });
    }

    for (auto& th : writers) th.join();
    stop = true;
    for (auto& th : readers) th.join();

    EXPECT_FALSE(errors.load());
}

TEST_F(DBTest, ManifestTrackingAndCompactionCleanup) {
    Options options;
    options.write_buffer_size = 50;
    delete db_;
    std::filesystem::remove_all("test_db");

    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    for (int i = 0; i < 60; ++i) {
        db_->Put("mkey_" + std::to_string(i), "mval_" + std::to_string(i));
    }

    std::string manifest_path = "test_db/MANIFEST";
    ASSERT_TRUE(std::filesystem::exists(manifest_path));

    std::ifstream mfile(manifest_path);
    std::string header;
    std::getline(mfile, header);
    EXPECT_EQ(header, "MANIFEST_V1");

    std::vector<std::string> before_ssts;
    std::string line;
    while (std::getline(mfile, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) before_ssts.push_back(line);
    }
    mfile.close();
    EXPECT_GT(before_ssts.size(), 1u);

    for (const auto& sst : before_ssts) {
        EXPECT_TRUE(std::filesystem::exists("test_db/" + sst));
    }

    db_->Compact();

    std::ifstream mfile2(manifest_path);
    std::getline(mfile2, header);
    EXPECT_EQ(header, "MANIFEST_V1");

    std::vector<std::string> after_ssts;
    while (std::getline(mfile2, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) after_ssts.push_back(line);
    }
    mfile2.close();

    EXPECT_EQ(after_ssts.size(), 1u);
    EXPECT_TRUE(std::filesystem::exists("test_db/" + after_ssts[0]));

    for (const auto& old_sst : before_ssts) {
        if (old_sst != after_ssts[0]) {
            EXPECT_FALSE(std::filesystem::exists("test_db/" + old_sst));
        }
    }

    delete db_;
    db_ = nullptr;

    s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    for (int i = 0; i < 60; ++i) {
        std::string val;
        s = db_->Get("mkey_" + std::to_string(i), &val);
        ASSERT_TRUE(s.ok()) << "Key missing after restart: mkey_" << i;
        EXPECT_EQ(val, "mval_" + std::to_string(i));
    }
}

TEST_F(DBTest, FaultInjection_MalformedWAL) {
    delete db_;
    db_ = nullptr;
    std::error_code ec;
    std::filesystem::remove_all("test_db", ec);
    std::filesystem::create_directories("test_db", ec);

    std::ofstream out("test_db/wal.log", std::ios::binary);
    uint8_t type = 1; // Put
    out.write(reinterpret_cast<const char*>(&type), 1);
    char len_buf[4];
    EncodeFixed32(len_buf, 0x00FFFFFF); // 16MB length
    out.write(len_buf, 4);
    out.write("short_key", 9); // truncated
    out.close();

    Options options;
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    std::string val;
    s = db_->Get("short_key", &val);
    ASSERT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, FaultInjection_TruncatedSSTable) {
    delete db_;
    db_ = nullptr;
    std::error_code ec;
    std::filesystem::remove_all("test_db", ec);
    std::filesystem::create_directories("test_db", ec);

    std::ofstream out("test_db/000001.sst", std::ios::binary);
    out.write("bad", 3);
    out.close();

    Options options;
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    std::string val;
    s = db_->Get("any_key", &val);
    ASSERT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, FaultInjection_IncompleteCompactionArtifacts) {
    Options options;
    options.write_buffer_size = 50;
    delete db_;
    std::filesystem::remove_all("test_db");
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    for (int i = 0; i < 20; ++i) {
        db_->Put("valid_" + std::to_string(i), "data_" + std::to_string(i));
    }

    delete db_;
    db_ = nullptr;

    std::ofstream tmp_sst("test_db/compact_tmp.sst", std::ios::binary);
    tmp_sst.write("incomplete_temp_sstable_content", 31);
    tmp_sst.close();

    std::ofstream tmp_manifest("test_db/MANIFEST.tmp", std::ios::trunc);
    tmp_manifest << "MANIFEST_V1\ncorrupt_unfinished_manifest\n";
    tmp_manifest.close();

    s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    EXPECT_FALSE(std::filesystem::exists("test_db/compact_tmp.sst"));
    EXPECT_FALSE(std::filesystem::exists("test_db/MANIFEST.tmp"));

    for (int i = 0; i < 20; ++i) {
        std::string val;
        s = db_->Get("valid_" + std::to_string(i), &val);
        ASSERT_TRUE(s.ok());
        EXPECT_EQ(val, "data_" + std::to_string(i));
    }
}

TEST_F(DBTest, FaultInjection_MissingManifestFallback) {
    delete db_;
    std::filesystem::remove_all("test_db");

    Options options;
    options.write_buffer_size = 50;
    Status s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());

    for (int i = 0; i < 20; ++i) {
        db_->Put("fkey_" + std::to_string(i), "fval_" + std::to_string(i));
    }

    delete db_;
    db_ = nullptr;

    std::error_code ec;
    std::filesystem::remove("test_db/MANIFEST", ec);
    ASSERT_FALSE(std::filesystem::exists("test_db/MANIFEST"));

    s = DB::Open(options, "test_db", &db_);
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE(std::filesystem::exists("test_db/MANIFEST"));

    for (int i = 0; i < 20; ++i) {
        std::string val;
        s = db_->Get("fkey_" + std::to_string(i), &val);
        ASSERT_TRUE(s.ok());
        EXPECT_EQ(val, "fval_" + std::to_string(i));
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

