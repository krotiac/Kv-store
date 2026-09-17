#ifndef KVSTORE_DB_H_
#define KVSTORE_DB_H_

#include <string>
#include "status.h"
#include "options.h"

namespace kvstore {

class DB {
public:
    virtual ~DB() = default;

    // Open the database with the specified "name" (which represents a directory on disk).
    // Stores a pointer to a heap-allocated database in *dbptr and returns OK on success.
    static Status Open(const Options& options, const std::string& name, DB** dbptr);

    // Set the database entry for "key" to "value".
    virtual Status Put(const std::string& key, const std::string& value) = 0;

    // Remove the database entry for "key".
    virtual Status Delete(const std::string& key) = 0;

    // If the database contains an entry for "key" store the corresponding value in *value and return OK.
    virtual Status Get(const std::string& key, std::string* value) = 0;

    // Phase 7: Compaction
    virtual void Compact() = 0;
};

} // namespace kvstore

#endif // KVSTORE_DB_H_

