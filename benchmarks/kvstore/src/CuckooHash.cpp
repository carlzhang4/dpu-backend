#include "CuckooHash.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <stdexcept>
using namespace std;

CuckooHash::CuckooHash(size_t table_size, hash_function* hash_funcs, size_t hash_func_num)
    : table_size(table_size), hash_functions(hash_funcs), hash_function_num(hash_func_num) {
    if (table_size == 0 || hash_func_num == 0 || !hash_funcs) {
        throw invalid_argument("Invalid parameters for CuckooHash constructor.");
    }
    
    entry_used.resize(hash_func_num, vector<bool>(table_size, false));
    table.resize(hash_func_num);
    //std::cout << "CuckooHash initialized with table size: " << table_size << " and " << hash_func_num << " hash functions." << std::endl;
    for (size_t i = 0; i < hash_func_num; ++i) {
        table[i] = new kv_storage*[table_size];
        if (!table[i]) {
            throw runtime_error("Memory allocation failed for CuckooHash table.");
        }
        for (size_t j = 0; j < table_size; ++j) {
            table[i][j] = new kv_storage();
            if (!table[i][j]) {
                throw runtime_error("Memory allocation failed for CuckooHash table.");
            }
            memset(table[i][j]->key, 0, KEY_SIZE);
            memset(table[i][j]->value, 0, VALUE_SIZE);
        }
    }
    //std::cout << "CuckooHash table initialized." << std::endl;
    entry_sizes.resize(hash_func_num, 0);
}
CuckooHash::~CuckooHash() {
    for (size_t i = 0; i < hash_function_num; ++i) {
        for (size_t j = 0; j < table_size; ++j) {
            delete table[i][j];
        }
        delete[] table[i];
    }
    table.clear();
    entry_used.clear();
    entry_sizes.clear();
    hash_functions = nullptr;
    table_size = 0;
    hash_function_num = 0;
    //cout << "CuckooHash destroyed." << endl;
}

int CuckooHash::insert(const char* key, const char* value, size_t key_size, size_t value_size) {
    if (key_size > MAX_KEY_SIZE || value_size > MAX_VALUE_SIZE) {
        return -1; // Key or value size exceeds maximum limits
    }
    struct kv_storage new_entry,tmp;
    memcpy(new_entry.key, key, key_size);
    memcpy(new_entry.value, value, value_size);
    for(size_t i=0;i< MAX_LOOPS; i++){
        for(size_t j=0;j<hash_function_num;j++){
            size_t index = hash_functions[j](new_entry.key, key_size) % table_size;
            if (!entry_used[j][index]) {
                // If the slot is empty, insert the new entry
                memcpy(table[j][index]->key, new_entry.key, key_size);
                memcpy(table[j][index]->value, new_entry.value, value_size);
                entry_used[j][index] = true;
                entry_sizes[j]++;
                return 0; // Insertion successful
            } else {
                // If the slot is occupied, evict the existing entry
                memcpy(&tmp, table[j][index], sizeof(kv_storage));
                memcpy(table[j][index]->key, new_entry.key, key_size);
                memcpy(table[j][index]->value, new_entry.value, value_size);
                new_entry = tmp; // Prepare for the next iteration
            }
            
        }
    }
    //std::cerr << "Insertion failed after " << MAX_LOOPS << " attempts. All slots are occupied." << std::endl;
    return -1; // All slots are occupied, insertion failed
}

void CuckooHash::print_entry_sizes() {
    for (size_t i = 0; i < hash_function_num; ++i) {
        //std::cout << "Hash function " << i << " has " << entry_sizes[i] << " entries." << std::endl;
        std::cout <<   entry_sizes[i] << " ";
    }
    std::cout << std::endl;
}