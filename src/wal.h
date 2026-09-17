#ifndef KVSTORE_WAL_H_
#define KVSTORE_WAL_H_

#include "kvstore/status.h"
#include "memtable.h"
#include <string>
#include <fstream>

namespace kvstore {

class WAL {
public:
    WAL() = default;
    ~WAL();

    // Open the WAL file for appending.
    Status OpenForAppend(const std::string& path);

    // Append operations to the WAL.
    Status AppendPut(const std::string& key, const std::string& value);
    Status AppendDelete(const std::string& key);

    // Truncate/delete the WAL file (used after a successful data.bin snapshot).
    void Clear();

    // Replay the WAL into the given MemTable.
    // Safely stops if it encounters a truncated or incomplete record.
    static void Replay(const std::string& path, MemTable* memtable);

private:
    std::string path_;
    std::ofstream out_;
};

} // namespace kvstore

#endif // KVSTORE_WAL_H_
