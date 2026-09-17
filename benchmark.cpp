#include "db.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <filesystem>
#include <random>
#include <thread>
#include <iomanip>

using namespace kvstore;

struct BenchmarkTimer {
    std::chrono::high_resolution_clock::time_point start;
    BenchmarkTimer() { reset(); }
    void reset() { start = std::chrono::high_resolution_clock::now(); }
    double elapsed_sec() const {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double>(end - start).count();
    }
};

int CountSSTables(const std::string& dbname) {
    int count = 0;
    if (std::filesystem::exists(dbname)) {
        for (const auto& entry : std::filesystem::directory_iterator(dbname)) {
            if (entry.path().extension() == ".sst") {
                count++;
            }
        }
    }
    return count;
}

void PrintMetrics(const std::string& name, int num_ops, double elapsed_sec) {
    double ops_per_sec = num_ops / elapsed_sec;
    double avg_latency_us = (elapsed_sec * 1e6) / num_ops;
    std::cout << std::left << std::setw(28) << name 
              << ": " << std::right << std::setw(10) << static_cast<long long>(ops_per_sec) << " ops/sec"
              << "  (" << std::fixed << std::setprecision(2) << avg_latency_us << " us/op)"
              << std::endl;
}

void RunComprehensiveBenchmark() {
    const int num_ops = 20000;
    const std::string dbname = "bench_db";
    std::filesystem::remove_all(dbname);

    DB* db = nullptr;
    Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 128 * 1024; // 128KB buffer to generate realistic SSTables

    Status s = DB::Open(options, dbname, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB for benchmark: " << s.ToString() << std::endl;
        return;
    }

    std::cout << "========================================================\n";
    std::cout << "     KVStore Production-Quality Benchmark Runner        \n";
    std::cout << "========================================================\n";
    std::cout << "Operations per test : " << num_ops << "\n";
    std::cout << "MemTable Buffer Size: 512 KB\n\n";

    // 1. Sequential PUT
    std::vector<std::string> seq_keys(num_ops);
    std::vector<std::string> seq_vals(num_ops);
    for (int i = 0; i < num_ops; ++i) {
        seq_keys[i] = "seq_key_" + std::to_string(i);
        seq_vals[i] = "seq_val_" + std::to_string(i) + "_fixed_payload_bytes_32b";
    }

    BenchmarkTimer timer;
    for (int i = 0; i < num_ops; ++i) {
        db->Put(seq_keys[i], seq_vals[i]);
    }
    PrintMetrics("1. Sequential PUT", num_ops, timer.elapsed_sec());

    // 2. Random PUT
    std::vector<std::string> rnd_keys(num_ops);
    std::vector<std::string> rnd_vals(num_ops);
    std::mt19937 rng(1337);
    for (int i = 0; i < num_ops; ++i) {
        rnd_keys[i] = "rnd_key_" + std::to_string(rng() % (num_ops * 2));
        rnd_vals[i] = "rnd_val_" + std::to_string(i);
    }

    timer.reset();
    for (int i = 0; i < num_ops; ++i) {
        db->Put(rnd_keys[i], rnd_vals[i]);
    }
    PrintMetrics("2. Random PUT", num_ops, timer.elapsed_sec());

    // 3. Sequential GET
    timer.reset();
    for (int i = 0; i < num_ops; ++i) {
        std::string val;
        db->Get(seq_keys[i], &val);
    }
    PrintMetrics("3. Sequential GET", num_ops, timer.elapsed_sec());

    // 4. Random GET
    timer.reset();
    for (int i = 0; i < num_ops; ++i) {
        std::string val;
        db->Get(rnd_keys[i], &val);
    }
    PrintMetrics("4. Random GET", num_ops, timer.elapsed_sec());

    // 5. Multithreaded Concurrent GET
    const int num_threads = 4;
    const int ops_per_thread = num_ops / num_threads;
    std::vector<std::thread> readers;
    timer.reset();
    for (int t = 0; t < num_threads; ++t) {
        readers.emplace_back([db, &seq_keys, t, ops_per_thread]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                int idx = (t * ops_per_thread + i);
                std::string val;
                db->Get(seq_keys[idx], &val);
            }
        });
    }
    for (auto& th : readers) th.join();
    PrintMetrics("5. Multithreaded GET (4T)", num_ops, timer.elapsed_sec());

    // 6. Mixed Workload (50% Read / 50% Write)
    timer.reset();
    for (int i = 0; i < num_ops; ++i) {
        if (i % 2 == 0) {
            db->Put("mixed_key_" + std::to_string(i), "mixed_val");
        } else {
            std::string val;
            db->Get(seq_keys[i % num_ops], &val);
        }
    }
    PrintMetrics("6. Mixed Read/Write (50/50)", num_ops, timer.elapsed_sec());

    // 7. DELETE Throughput
    timer.reset();
    for (int i = 0; i < num_ops; ++i) {
        db->Delete(seq_keys[i]);
    }
    PrintMetrics("7. DELETE", num_ops, timer.elapsed_sec());

    // 8. Compaction Performance
    int ssts_before = CountSSTables(dbname);
    std::cout << "\n--- Compaction Benchmark ---\n";
    std::cout << "Active SSTables before compaction: " << ssts_before << "\n";

    timer.reset();
    db->Compact();
    double compact_time = timer.elapsed_sec();

    int ssts_after = CountSSTables(dbname);
    std::cout << "Active SSTables after compaction : " << ssts_after << "\n";
    std::cout << "Compaction Latency               : " << std::fixed << std::setprecision(4) 
              << compact_time << " seconds\n";
    std::cout << "SSTable Reduction                : " << ssts_before << " -> " << ssts_after << " files\n";
    std::cout << "========================================================\n";

    delete db;
}

int main() {
    RunComprehensiveBenchmark();
    return 0;
}
