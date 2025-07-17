#include <iostream>
#include <cstring>
#include <cassert>
#include <cstdint>
#include "kvstore.h"

uint64_t hash_func1(const char* key, size_t len) {
    uint64_t hash=0; 
    memcpy(&hash, key, len);
    return hash;
}


void init_key_value_extent(KVStore_server &store, uint64_t max_entrys) {
    bool insert_state = true;
    for(uint64_t i = 0; i < max_entrys; ++i) {
        insert_state=store.insert((const char*)&i,(const char*)&i, sizeof(i), sizeof(i));
        assert(insert_state && "Failed to insert initial key-value pair.");
    }
}


int main() {
    uint64_t max_entrys = 100000;
    hash_function hash_funcs[] = {hash_func1}; 
    KVStore_server store(max_entrys, 1, hash_funcs);
    init_key_value_extent(store, max_entrys);

    kv_entry* entries = store.get_entry_ptr();
    key_value_extent* extents = store.get_extent_ptr();

    KVStore_client client(max_entrys, entries, extents, 1, hash_funcs);

    for (uint64_t i = 0; i < max_entrys; ++i) {
        key_value_extent* extent = client.get_entry((const char*)&i, sizeof(i));
        assert(extent != nullptr && "Failed to retrieve key-value pair.");
        //std::cout << "Key: " << *(uint64_t*)extent->context << ", Value: " << *(uint64_t*)(extent->context+entries->key_size)<< std::endl;
        assert(memcmp(extent->context, &i, sizeof(entries->key_size)) == 0 && "Key mismatch.");
        assert(memcmp(extent->context+entries->key_size, &i, sizeof(entries->key_value_size-entries->key_size)) == 0 && "Value mismatch.");
    }
    
    std::cout << "All key-value pairs verified successfully." << std::endl;
    return 0;
}