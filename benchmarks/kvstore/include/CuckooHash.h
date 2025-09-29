#include "KVStore_Config.h"
#include "kvstore.h"

#ifndef CUCKOOHASH_H
#define CUCKOOHASH_H

#define MAX_LOOPS 1000

#include <vector>

class CuckooHash {
public:
    CuckooHash(size_t table_size, hash_function* hash_funcs, size_t hash_func_num);
    ~CuckooHash();
    int insert(const char* key, const char* value, size_t key_size, size_t value_size);
    void print_entry_sizes();
    
private:
    size_t table_size;
    std::vector<kv_storage**> table;
    std::vector<std::vector<bool>> entry_used;
    hash_function* hash_functions;
    std::vector<size_t> entry_sizes;
    size_t hash_function_num;
};



#endif // CUCKOOHASH_H