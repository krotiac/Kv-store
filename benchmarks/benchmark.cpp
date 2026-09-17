#include "kvstore/db.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <filesystem>

using namespace kvstore;

void RunBenchmark(int num_ops) {
    std::filesystem::remove_all("bench_db");
    
    DB* db = nullptr;
    Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 1024 * 1024; // 1MB for more flushes
    
    Status s = DB::Open(options, "bench_db", &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB for benchmark\n";
        return;
    }

    std::vector<std::string> keys(num_ops);
    std::vector<std::string> vals(num_ops);
    for (int i = 0; i < num_ops; ++i) {
        keys[i] = "key_" + std::to_string(i);
        vals[i] = "val_" + std::to_string(i) + "_padding_to_make_it_longer";
    }

    // Benchmark PUT
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_ops; ++i) {
        db->Put(keys[i], vals[i]);
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> put_time = end - start;
    std::cout << "PUT: " << num_ops / put_time.count() << " ops/sec\n";

    // Benchmark GET
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_ops; ++i) {
        std::string val;
        db->Get(keys[i], &val);
    }
    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> get_time = end - start;
    std::cout << "GET: " << num_ops / get_time.count() << " ops/sec\n";

    // Benchmark DELETE
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_ops; ++i) {
        db->Delete(keys[i]);
    }
    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> del_time = end - start;
    std::cout << "DELETE: " << num_ops / del_time.count() << " ops/sec\n";

    // Benchmark Compaction
    start = std::chrono::high_resolution_clock::now();
    db->Compact();
    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> comp_time = end - start;
    std::cout << "COMPACTION Time: " << comp_time.count() << " sec\n";

    delete db;
}

int main() {
    std::cout << "Running benchmark with 100,000 operations...\n";
    RunBenchmark(100000);
    return 0;
}
