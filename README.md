# ⚡ KVStore: An LSM-Tree Key-Value Storage Engine in C++17

A high-performance, persistent, thread-safe embedded key-value storage engine built from scratch in modern C++17, inspired by the architectures of **LevelDB** and **RocksDB**.

This project demonstrates core systems software engineering concepts: **low-level binary I/O, custom concurrent data structures, write-ahead logging (WAL), crash recovery, immutable sorted disk storage (SSTables), sparse indexing, atomic metadata transitions (MANIFEST), and multi-way merge compaction**.

---

## 🏗️ System Architecture

```
                      +-------------------+
                      |    Application    |
                      +-------------------+
                                |
                         DB API (Put, Get, Delete, Compact)
                                |
            +-------------------+-------------------+
            | (Write Path)                          | (Read Path)
            v                                       v
    +---------------+                       +---------------+
    |  WAL (.log)   |                       |   MemTable    | (1. In-memory lookup)
    | (Append-Only) |                       |  (SkipList)   |
    +---------------+                       +---------------+
            |                                       | (Miss / Not in RAM)
            v (On buffer full / shutdown)           v
    +---------------+                       +---------------+
    | Flush Engine  |                       |  SSTable #N   | (2. Newest to oldest)
    +---------------+                       | (Sparse Index)|
            |                               +---------------+
            v                                       |
    +---------------+                               v
    | Immutable SST |                       +---------------+
    | (.sst files)  |<----------------------|  SSTable #1   |
    +---------------+                       +---------------+
            |
            +--- Compaction Engine ---> [ New Merged SSTable ]
                         |
                 Atomic MANIFEST Commit
```

---

## 🚀 Key Features & Internals

### 1. In-Memory SkipList (MemTable)
* **Probabilistic Balancing:** Implements a multi-level SkipList providing $O(\log N)$ expected time for insertions, updates, lookups, and deletions without the complex rotational rebalancing of Red-Black Trees.
* **Sorted Iteration:** Emits entries in lexicographical key order during disk flushes.
* **Exact Byte Accounting:** Dynamically tracks exact memory usage (`ApproximateSize()`) across overwrites and tombstone insertions to reliably trigger SSTable flushes.

### 2. Durability & Crash Recovery (Write-Ahead Log)
* **Append-Only Logging:** Every write (`Put` or `Delete`) is serialized and written to disk before modifying the in-memory MemTable, guaranteeing zero data loss on unexpected termination.
* **Binary Encoding:** Uses compact Little-Endian fixed-width length prefixes (`EncodeFixed32` / `DecodeFixed32`).
* **Fault-Tolerant Replay:** Replays logs sequentially upon startup and cleanly ignores incomplete or torn tail records resulting from hard crashes mid-flush.

### 3. Immutable Sorted String Tables (SSTables)
* **Immutable On-Disk Format:** Flushed files are immutable, eliminating write amplification from in-place updates.
* **Data Block + Sparse Index + Footer:**
  * Sequential data records: `[KeyLen][Key][ValLen][Val][TombstoneFlag]`
  * Sampled sparse index block: Maps periodic keys to byte offsets.
  * 4-byte footer: Encodes the offset of the index block for constant-time index location on open.
* **Tombstone Semantics:** Deletions write a tombstone marker that shadows older versions across previous SSTables until compaction.

### 4. High-Performance Sparse Indexing
* **Sub-linear Search:** Only the sparse index is held in RAM. Reads pinpoint target byte ranges on disk via in-memory binary search, scanning at most a single small block.
* **Fast Boundary Rejection:** Automatically rejects out-of-range keys ($O(1)$) if `key < min_key` without performing any disk I/O.

### 5. Crash-Safe Compaction & MANIFEST Metadata
* **Multi-Way Merge:** Combines multiple overlapping SSTables into a single consolidated SSTable, purging obsolete overwritten versions and reclaiming disk space from deleted tombstones.
* **Atomic MANIFEST State Transition:** 
  1. Writes compacted data to a temporary file (`compact_tmp.sst`).
  2. Commits active table set changes to `MANIFEST.tmp`.
  3. Atomically replaces `MANIFEST` using filesystem-level atomic rename (`MoveFileEx` / POSIX `rename`).
  4. Only deletes obsolete SSTables *after* the new state is safely committed.
  5. Uncommitted artifacts from interrupted compactions are quarantined and cleaned on recovery.

### 6. Thread-Safe Concurrency Model
* **Multi-Reader / Exclusive-Writer:** Synchronized via `std::shared_mutex` (`shared_lock` for `Get`, `unique_lock` for `Put`, `Delete`, `Compact`).
* **Stream-Less Positional Reads:** `SSTableReader` does not share a mutable file pointer across threads. Each concurrent query operates on independent streams by RAII, eliminating race conditions during parallel reads.

---

## 📊 Benchmark Results

Benchmarked on **Windows (AMD64)** with 20,000 operations per workload against a 128 KB MemTable buffer (triggering dozens of SSTable flushes):

| Workload | Throughput | Average Latency |
| :--- | :---: | :---: |
| **Sequential PUT** | **100,252 ops/sec** | **9.97 $\mu s$ / op** |
| **Random PUT** | **72,339 ops/sec** | **13.82 $\mu s$ / op** |
| **Random GET** | **4,369 ops/sec** | **228.88 $\mu s$ / op** |
| **Multithreaded GET (4 Threads)** | **3,067 ops/sec** | **325.95 $\mu s$ / op** |
| **Mixed Read/Write (50% / 50%)** | **2,084 ops/sec** | **479.62 $\mu s$ / op** |
| **DELETE** | **74,099 ops/sec** | **13.50 $\mu s$ / op** |
| **Compaction Latency** | **19 $\to$ 1 SSTables in 0.2257 sec** | **94.7% File Reduction** |

---

## 📂 Project Structure

```text
kvstore_flat/
├── CMakeLists.txt        # Build definitions with GoogleTest & CTest integration
├── README.md             # Architecture overview & documentation
├── benchmark.cpp         # Comprehensive throughput & latency benchmark suite
├── coding.h              # Little-Endian binary encoding / decoding utilities
├── db.h                  # Clean public C++ API
├── db.cpp                # Core orchestrator: concurrency, manifest, compaction
├── db_test.cpp           # 20-test GoogleTest suite (unit, concurrent, recovery)
├── memtable.h            # In-memory SkipList interface & sorted iterator
├── memtable.cpp          # SkipList implementation & memory byte accounting
├── options.h             # Storage engine configuration (buffer sizes, etc.)
├── status.h              # Status return codes (OK, NotFound, IOError, etc.)
├── sstable.h             # SSTable Writer & stream-less Reader interface
├── sstable.cpp           # SSTable file layout, sparse indexing & block scans
├── wal.h                 # Write-Ahead Log interface
└── wal.cpp               # Append-only binary log formatting & crash replay
```

---

## 🛠️ Build & Run Instructions

### Prerequisites
* **C++17 compliant compiler** (GCC 9+, Clang 10+, or MSVC 2019+)
* **CMake 3.14+**

### 1. Build the Project
```bash
# Configure build directory
cmake -S . -B build -G "MinGW Makefiles"

# Compile library, test suite, and benchmark
cmake --build build
```

### 2. Run the Test Suite (20 Tests)
```bash
# Direct test execution
./build/db_test.exe

# Or via CTest
ctest --test-dir build --output-on-failure
```

#### Test Coverage Overview:
* **Basic Functionality:** `PutAndGet`, `GetNotFound`, `DeleteKey`, `UpdateKey`
* **Durability & Persistence:** `PersistenceAcrossRestarts`, `EmptyDatabasePersistence`
* **Crash Recovery:** `WALRecovery`, `TruncatedWAL`
* **Data Structures:** `MemTableTest.SkipListOperations`, `MemTableTest.ApproximateSizeAccounting`
* **Concurrency:** `ConcurrentSSTableReads`, `ConcurrentReaders`, `ConcurrentWriters`, `ConcurrentMixedReadWrite`
* **Compaction & Metadata:** `SSTablesAndCompaction`, `ManifestTrackingAndCompactionCleanup`
* **Fault Injection:** `FaultInjection_MalformedWAL`, `FaultInjection_TruncatedSSTable`, `FaultInjection_IncompleteCompactionArtifacts`, `FaultInjection_MissingManifestFallback`

### 3. Run the Benchmark
```bash
./build/db_bench.exe
```

---

## 💡 System Design & Interview Talking Points

### Q: Why use an LSM-Tree instead of a B+Tree?
* **Write Throughput:** B+Trees perform random in-place updates across pages, causing disk fragmentation, heavy write amplification, and page splits. LSM-Trees convert random writes into sequential writes in memory (MemTable) and append-only disk files (WAL & SSTables), saturating hardware write bandwidth.
* **SSD Friendliness:** Sequential appends minimize SSD wear-leveling overhead compared to random 4KB page overwrites.

### Q: Why must the WAL append precede the MemTable update?
* If the MemTable were updated first and the process crashed before the WAL append completed, the client would have observed uncommitted state that disappears upon reboot, violating durability (ACID). Appending and flushing the WAL first ensures durability guarantees.

### Q: How do deletes work if SSTables are immutable?
* Rather than seeking through disk to erase bytes, a delete writes a **Tombstone** record to the MemTable and WAL. During reads, the tombstone masks older versions of the key in underlying SSTables. The dead record is physically purged from disk only when **Compaction** merges the affected SSTables.

### Q: How is crash safety achieved during Compaction?
* Compaction never modifies existing SSTables in-place. It writes the merged data to a new isolated file and performs an **atomic metadata commit** via `MANIFEST.tmp` $\to$ `MANIFEST`. If power is lost mid-compaction, the database recovers using the old, valid `MANIFEST`, ignoring the incomplete temporary SSTable.

---

## 📄 License
This project is licensed under the MIT License - open for educational and portfolio demonstration use.
