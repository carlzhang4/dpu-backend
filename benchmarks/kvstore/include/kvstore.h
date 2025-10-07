#ifndef KVSTORE_H
#define KVSTORE_H

#include <iostream>
#include "KVStore_Config.h"

#define MAX_KEY_SIZE 64
#define MAX_VALUE_SIZE 256

struct kv_storage {
    char key[KEY_SIZE];
    char value[VALUE_SIZE];
};

struct kv_entry {
    bool in_use;
    uint8_t hash_func_used;
    void* key_value_pointer;
    size_t key_value_size;
    size_t key_size;
    size_t value_size;
};

struct key_value_extent {
    char context[MAX_KEY_SIZE+MAX_VALUE_SIZE];
};

using hash_function = uint64_t (*)(const char*, size_t);

class KVStore_server {
public:
    KVStore_server(uint64_t max_entrys, size_t hash_func_num, hash_function* hash_funcs);
    KVStore_server(uint64_t max_entrys, size_t hash_func_num, hash_function* hash_funcs, void* buf);
    
    ~KVStore_server();
    bool insert(const char* key, const char* value, size_t key_size, size_t value_size);
    kv_entry* get_entry_ptr();
    key_value_extent* get_extent_ptr();


private:
    kv_entry* kv_entries;
    key_value_extent* kv_extents;
    size_t kv_entry_count;
    size_t max_entrys;
    size_t hash_function_num;
    hash_function* hash_functions;
};

class KVStore_client {
public:
    KVStore_client(uint64_t max_entrys,kv_entry* entries, key_value_extent* extents, size_t hash_func_num, hash_function* hash_funcs);
    ~KVStore_client();
    key_value_extent* get_entry(const char* key, size_t key_size);

private:
    kv_entry* kv_entries;
    size_t max_entrys;
    key_value_extent* kv_extents;
    size_t hash_function_num;
    hash_function* hash_functions;
};



#endif // KVSTORE_H