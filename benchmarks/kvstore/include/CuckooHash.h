#include "KVStore_Config.h"
#include "kvstore.h"
#include <string>
#include <fcntl.h>

#ifndef CUCKOOHASH_H
#define CUCKOOHASH_H

#define MAX_LOOPS 10000
#define MAX_KEY_SIZE 32

#include <vector>

// 反序列化后的结构
struct UpdateMessage {
    std::string key;
    std::vector<std::pair<std::string, std::string>> values; // {field, value}
};

struct KVhashtable{
    char key[MAX_KEY_SIZE];
    bool valid;
    uint64_t value_offset;
    uint64_t key_size;
    uint64_t value_size;
};

class CuckooHash {
public:
    typedef std::pair<std::string, std::string> KVPair;
    CuckooHash(size_t table_size, hash_function* hash_funcs, size_t hash_func_num);
    ~CuckooHash();
    int insert(const char* key, const char* value, size_t key_size, size_t value_size);
    int insert(const UpdateMessage& msg);
    int Read(const std::string &key,std::vector<KVPair> &result);
    void print_entry_sizes();
    int receive_and_update(int sockfd);
    void parse_values_fixed(const std::vector<char>& data, std::vector<KVPair>& out);
    
private:
    

    // struct kv_update_request {
    //     size_t total_size;
    //     size_t key_size;
    //     std::string key;
    //     std::vector<KVPair> values;
        
    // };

  

    size_t table_size;
    std::vector<KVhashtable*> KVHashTable_vec;
    std::vector<uint64_t> KVhashtableSize;
    // std::vector<std::vector<bool>> entry_used;
    std::vector<char*> value_storage;
    std::vector<size_t> value_storage_used;
    hash_function* hash_functions;
    std::vector<size_t> entry_sizes;
    size_t hash_function_num;
    FILE *fp;
};



#endif // CUCKOOHASH_H