#include "kvstore/db.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <string>

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

#include "utils/coding.h"

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

#include "../src/memtable.h"

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

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
