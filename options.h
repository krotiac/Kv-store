#ifndef KVSTORE_OPTIONS_H_
#define KVSTORE_OPTIONS_H_

#include <cstddef>

namespace kvstore {

struct Options {
    // If true, the database will be created if it is missing.
    bool create_if_missing = false;
    
    // If true, an error is raised if the database already exists.
    bool error_if_exists = false;

    // Max size of memtable before flushing (will be used in later phases)
    size_t write_buffer_size = 4 * 1024 * 1024; 
};

} // namespace kvstore

#endif // KVSTORE_OPTIONS_H_

