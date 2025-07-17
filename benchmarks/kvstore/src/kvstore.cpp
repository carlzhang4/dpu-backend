#include <iostream>
#include <kvstore.h>
#include <string.h>

KVStore_server::KVStore_server(uint64_t max_entrys, size_t hash_func_num, hash_function* hash_funcs) {
    this->max_entrys = max_entrys;
    kv_entries = new kv_entry[max_entrys];
    kv_extents = new key_value_extent[max_entrys];
    for(size_t i = 0; i < max_entrys; ++i) {
        kv_entries[i].in_use = false;
        kv_entries[i].key_value_pointer = nullptr;
        kv_entries[i].key_value_size = 0;
        kv_entries[i].key_size = 0;
    }
    kv_entry_count = 0;
    this->hash_function_num = hash_func_num;
    hash_functions = new hash_function[hash_func_num];
    for (size_t i = 0; i < hash_func_num; ++i) {
        hash_functions[i] = hash_funcs[i];
    }
    std::cout << "KVStore server initialized with max entries: " << max_entrys << std::endl;
}

KVStore_server::KVStore_server(uint64_t max_entrys, size_t hash_func_num, hash_function* hash_funcs, void* buf){
    this->max_entrys = max_entrys;
    kv_entries = (kv_entry*)buf;
    kv_extents = (key_value_extent*)((char*)buf+max_entrys*sizeof(kv_entry));
    for(size_t i = 0; i < max_entrys; ++i) {
        kv_entries[i].in_use = false;
        kv_entries[i].key_value_pointer = nullptr;
        kv_entries[i].key_value_size = 0;
        kv_entries[i].key_size = 0;
    }
    kv_entry_count = 0;
    this->hash_function_num = hash_func_num;
    hash_functions = new hash_function[hash_func_num];
    for (size_t i = 0; i < hash_func_num; ++i) {
        hash_functions[i] = hash_funcs[i];
    }
    std::cout << "KVStore server initialized with max entries: " << max_entrys << std::endl;

}
    

KVStore_server::~KVStore_server() {
    delete[] kv_entries;
    delete[] kv_extents;
}

bool KVStore_server::insert(const char* key, const char* value, size_t key_size, size_t value_size) {
    if (kv_entry_count >= max_entrys) {
        std::cerr << "KVStore is full, cannot insert new entry." << std::endl;
        return false;
    }
    
    for(size_t i = 0; i <hash_function_num; ++i) {
        uint64_t hash = hash_functions[i](key, key_size);
        size_t index = hash % max_entrys;
        //std::cout << "Inserting key: " << key << " using hash function " << i << " at index: " << index << std::endl;
        if (!kv_entries[index].in_use) {
            kv_entries[index].in_use = true;
            kv_entries[index].hash_func_used = i;
            kv_entries[index].key_value_pointer = &kv_extents[kv_entry_count];
            kv_entries[index].key_value_size = key_size + value_size;
            kv_entries[index].key_size = key_size;

            // strncpy(kv_extents[kv_entry_count].key, key, key_size);
            memcpy(kv_extents[kv_entry_count].context, key, key_size);
            // strncpy(kv_extents[kv_entry_count].value, value, value_size);
            memcpy(kv_extents[kv_entry_count].context+key_size, value, value_size);

            ++kv_entry_count;
            // if(index == 256){
            //     std::cout << "Hash function returned index 256" << std::endl;
            //     std::cout << "Key: " <<  *(uint64_t*)(kv_extents[kv_entry_count - 1].key) << std::endl;
            //     std::cout << "input key: " << *(uint64_t*)key << std::endl;
            //     std::cout << "key size : " << key_size << std::endl;
            //     exit(1);
           
            // }
            return true;
        }
    }
    
    std::cerr << "Failed to insert entry, all hash functions used." << std::endl;
    return false;
}

kv_entry* KVStore_server::get_entry_ptr() {
    return kv_entries;
}

key_value_extent* KVStore_server::get_extent_ptr() {
    return kv_extents;
}

KVStore_client::KVStore_client(uint64_t max_entrys,kv_entry* entries, key_value_extent* extents, size_t hash_func_num, hash_function* hash_funcs) {
    kv_entries = entries;
    kv_extents = extents;
    hash_function_num = hash_func_num;
    hash_functions = new hash_function[hash_func_num];
    max_entrys = max_entrys;
    for (size_t i = 0; i < hash_func_num; ++i) {
        hash_functions[i] = hash_funcs[i];
    }
}

KVStore_client::~KVStore_client() {
    delete[] hash_functions;
}

key_value_extent* KVStore_client::get_entry(const char* key, size_t key_size) {
    key_value_extent* result = new key_value_extent;
    for(int i = 0; i < hash_function_num; ++i) {
        uint64_t hash = hash_functions[i](key, key_size);
        size_t index = hash % max_entrys;
        //std::cout<< "Searching for key: " << key << " using hash function " << i << " at index: " << index << std::endl;

        if (kv_entries[index].in_use && kv_entries[index].key_size == key_size){
            void* kv_ptr = kv_entries[index].key_value_pointer;
            //strncmp(((key_value_extent*)(kv_entries[index].key_value_pointer))->key, key, key_size) == 0) {
            
            // strncpy(result->key, ((key_value_extent*)(kv_entries[index].key_value_pointer))->key, KEY_SIZE);
            // strncpy(result->value, ((key_value_extent*)(kv_entries[index].key_value_pointer))->value, VALUE_SIZE);
            memcpy(result->context, kv_ptr, key_size);
            memcpy(result->context+ key_size,(char*)kv_ptr +kv_entries[index].key_size, kv_entries[index].key_value_size - key_size);
            if(strncmp(result->context,key, key_size) != 0){
                break;
            }
            return result;
        }
    }
    delete result; // Clean up if no entry found
    return nullptr;

}