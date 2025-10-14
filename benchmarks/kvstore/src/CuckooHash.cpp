#include "CuckooHash.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <algorithm>
#include <errno.h>
#include <endian.h>

using namespace std;

// 固定大小布局参数（与构造函数中的分配保持一致）
static constexpr size_t MAX_FIELDS   = 10;
static constexpr size_t FIELD_CAP    = 28;   // 每个 field 的最大字节数
static constexpr size_t VALUE_CAP    = 4;  // 每个 value 的最大字节数
static constexpr size_t PER_PAIR_BYTES = FIELD_CAP + VALUE_CAP;
static constexpr size_t VALUE_SLOT_SIZE = MAX_FIELDS * PER_PAIR_BYTES;

// 将 values 打包到固定大小槽位：零填充，超长截断
static inline void pack_values_fixed(const std::vector<std::pair<std::string,std::string>>& values,
                                     char* dst /*size = VALUE_SLOT_SIZE*/) {
    std::memset(dst, 0, VALUE_SLOT_SIZE);
    const size_t n = std::min(values.size(), (size_t)MAX_FIELDS);
    for (size_t i = 0; i < n; ++i) {
        const auto& kv = values[i];
        char* pair_base = dst + i * PER_PAIR_BYTES;
        // field
        const size_t fcopy = std::min(kv.first.size(), (size_t)FIELD_CAP);
        std::memcpy(pair_base, kv.first.data(), fcopy);
        // value
        const size_t vcopy = std::min(kv.second.size(), (size_t)VALUE_CAP);
        std::memcpy(pair_base + FIELD_CAP, kv.second.data(), vcopy);
    }
}

static bool recv_all(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 非阻塞套接字可在此等待/重试
                continue;
            }
            return false;
        }
        if (n == 0) return false; // 对端关闭
        got += static_cast<size_t>(n);
    }
    return true;
}

static bool recv_u64_be(int fd, uint64_t& out) {
    uint64_t be = 0;
    if (!recv_all(fd, &be, sizeof(be))) return false;
    out = be64toh(be);
    return true;
}

static bool recv_string(int fd, std::string& s, uint64_t len) {
    if (len == 0) { s.clear(); return true; }
    if (len > static_cast<uint64_t>(SIZE_MAX)) return false; // 防止过大
    s.resize(static_cast<size_t>(len));
    return recv_all(fd, &s[0], static_cast<size_t>(len));
}

// 可选：对长度做上限约束，避免异常输入导致过大分配
struct Limits {
    uint64_t max_key_len   = 1ull << 20; // 1 MiB
    uint64_t max_pairs     = 1ull << 16; // 65536
    uint64_t max_field_len = 1ull << 20; // 1 MiB
    uint64_t max_value_len = 1ull << 20; // 1 MiB
};

bool recv_update_message(int fd, UpdateMessage& msg, const Limits& lim = {}) {
    msg = UpdateMessage{};

    uint64_t key_len = 0;
    if (!recv_u64_be(fd, key_len)) return false;
    if (key_len > lim.max_key_len) return false;
    if (!recv_string(fd, msg.key, key_len)) return false;

    uint64_t num_values = 0;
    if (!recv_u64_be(fd, num_values)) return false;
    if (num_values > lim.max_pairs) return false;

    msg.values.reserve(static_cast<size_t>(num_values));
    for (uint64_t i = 0; i < num_values; ++i) {
        uint64_t field_len = 0, value_len = 0;
        if (!recv_u64_be(fd, field_len)) return false;
        if (field_len > lim.max_field_len) return false;
        std::string field;
        if (!recv_string(fd, field, field_len)) return false;

        if (!recv_u64_be(fd, value_len)) return false;
        if (value_len > lim.max_value_len) return false;
        std::string value;
        if (!recv_string(fd, value, value_len)) return false;

        msg.values.emplace_back(std::move(field), std::move(value));
    }
    return true;
}

// 示例：从已建立的连接 fd 读取一条 Update 消息并打印
int handle_one_update(int fd) {
    UpdateMessage msg;
    if (!recv_update_message(fd, msg)) {
        std::cerr << "recv_update_message failed\n";
        return -1;
    }
    std::cout << "key: " << msg.key << "\n";
    for (size_t i = 0; i < msg.values.size(); ++i) {
        std::cout << "  [" << i << "] field=" << msg.values[i].first
                  << " value=" << msg.values[i].second << "\n";
    }
    return 0;
}

CuckooHash::CuckooHash(size_t table_size, hash_function* hash_funcs, size_t hash_func_num)
    : table_size(table_size), hash_functions(hash_funcs), hash_function_num(hash_func_num) {
    if (table_size == 0 || hash_func_num == 0 || !hash_funcs) {
        throw invalid_argument("Invalid parameters for CuckooHash constructor.");
    }


    KVHashTable_vec.resize(hash_func_num);
    KVhashtableSize.resize(hash_func_num,0);
    value_storage_used.resize(hash_func_num,0);
    value_storage.resize(hash_func_num);
    //std::cout << "CuckooHash initialized with table size: " << table_size << " and " << hash_func_num << " hash functions." << std::endl;
    for (size_t i = 0; i < hash_func_num; ++i) {
        KVHashTable_vec[i] =   new KVhashtable[table_size];
        std::cout << "Allocating CuckooHash table " << i << " with " << table_size << " slots." << std::endl;
        if (!KVHashTable_vec[i]) {
            throw runtime_error("Memory allocation failed for CuckooHash table.");
        }
        for (size_t j = 0; j < table_size; ++j) {
            // Initialize each struct element in-place (array already allocated above)
            std::memset(KVHashTable_vec[i][j].key, 0, MAX_KEY_SIZE);
            KVHashTable_vec[i][j].valid = false;
            KVHashTable_vec[i][j].value_offset = 0;
            KVHashTable_vec[i][j].key_size = 0;
            KVHashTable_vec[i][j].value_size = 0;
        }
         value_storage[i] = new char[table_size * VALUE_SLOT_SIZE];
        std::memset(value_storage[i], 0, table_size * VALUE_SLOT_SIZE);
    }
    //std::cout << "CuckooHash table initialized." << std::endl;
    entry_sizes.resize(hash_func_num, 0);
    //* open log file
    fp = fopen("cuckoo_hash.log","a");
    if (fp == NULL) {
		std::cerr << "Failed to open log file." << std::endl;
		return;
	}
}


CuckooHash::~CuckooHash() {
    for (size_t i = 0; i < hash_function_num; ++i) {
        // for (size_t j = 0; j < table_size; ++j) {
        //     delete KVHashTable_vec[i][j];
        // }
        delete[] KVHashTable_vec[i];
    }
    KVHashTable_vec.clear();
    for (size_t i = 0; i < hash_function_num; ++i) {
        delete[] value_storage[i];
    }
    value_storage.clear();
    entry_sizes.clear();
    value_storage_used.clear();
    KVhashtableSize.clear();
    hash_functions = nullptr;
    table_size = 0;
    hash_function_num = 0;
    //cout << "CuckooHash destroyed." << endl;
    fclose(fp);
}

int CuckooHash::insert(const char* key, const char* value, size_t key_size, size_t value_size) {
    if (key_size > MAX_KEY_SIZE || value_size > MAX_VALUE_SIZE) {
        return -1; // Key or value size exceeds maximum limits
    }
    // struct kv_storage new_entry,tmp;
    // memcpy(new_entry.key, key, key_size);
    // memcpy(new_entry.value, value, value_size);
    // for(size_t i=0;i< MAX_LOOPS; i++){
    //     for(size_t j=0;j<hash_function_num;j++){
    //         size_t index = hash_functions[j](new_entry.key, key_size) % table_size;
    //         if (!KVHashTable_vec[j][index].valid) {
    //             // If the slot is empty, insert the new entry
    //             memcpy(KVHashTable_vec[j][index].key, new_entry.key, key_size);
    //             // memcpy(KVHashTable_vec[j][index].value, new_entry.value, value_size);
    //             KVHashTable_vec[j][index].valid = true;
    //             entry_sizes[j]++;
    //             return 0; // Insertion successful
    //         } else {
    //             // If the slot is occupied, evict the existing entry
    //             memcpy(&tmp, KVHashTable_vec[j][index], sizeof(kv_storage));
    //             memcpy(KVHashTable_vec[j][index].key, new_entry.key, key_size);
    //             // memcpy(KVHashTable_vec[j][index].value, new_entry.value, value_size);
    //             new_entry = tmp; // Prepare for the next iteration
    //         }
            
    //     }
    // }
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

int CuckooHash::receive_and_update(int sockfd) {
    UpdateMessage msg;
    Limits lim;
    if (recv_update_message(sockfd, msg, lim)== -1) {
        std::cerr << "recv_update_message failed\n";
        return -1;
    }
    if(insert(msg) == -1) {
        std::cerr << "CuckooHash::insert failed for key: " << msg.key << std::endl;
        return -1;
    }
    //std::cout << "Received update for key: " << msg.key << " with " << msg.values.size() << " values.\n";
    //for (size_t i = 0; i < msg.values.size(); ++i) {
    //    std::cout << "  [" << i << "] field=" << msg.values[i].first
    //              << " value=" << msg.values[i].second << "\n";
    //}
    // 将接收到的更新应用到哈希表中
    // std::cout << "CuckooHash::receive_and_update: key=" << msg.key << std::endl;
    // for (const auto& kv : msg.values) {
    //     std::cout << "  field=" << kv.first <<" field value size :" << kv.second.size() << " value=" << kv.second << std::endl;
    // }
    return 0;
}


int CuckooHash::insert(const UpdateMessage& msg) {
    // 当前携带的键与 payload（固定大小）
    std::string cur_key = msg.key;
    size_t cur_key_size = std::min(cur_key.size(), (size_t)MAX_KEY_SIZE);

    // 准备初始 payload：将 msg.values 打包到固定槽位大小
    std::vector<char> cur_payload(VALUE_SLOT_SIZE);
    pack_values_fixed(msg.values, cur_payload.data());
    
    for (size_t loop = 0; loop < MAX_LOOPS; ++loop) {
        for (size_t t = 0; t < hash_function_num; ++t) {
            size_t idx = hash_functions[t](cur_key.c_str(), cur_key_size) % table_size;
            auto& slot = KVHashTable_vec[t][idx];
            const size_t off = idx * VALUE_SLOT_SIZE;
            char* slot_base = value_storage[t] + off;

            if (!slot.valid) {
                // 空槽：写入键与 payload
                std::memset(slot.key, 0, MAX_KEY_SIZE);
                std::memcpy(slot.key, cur_key.data(), cur_key_size);
                slot.valid = true;
                slot.key_size = cur_key_size;
                slot.value_offset = off; // 用 offset 作为索引
                std::memcpy(slot_base, cur_payload.data(), VALUE_SLOT_SIZE);
                entry_sizes[t]++;
                // std::cout << "Inserted key: " << std::string(slot.key, cur_key_size) << " at table " << t << ", index " << idx << std::endl;
                // getchar();
                fprintf(fp, "Inserted key: %s at table %zu, index %zu\n", std::string(slot.key, cur_key_size).c_str(), t, idx);
                return 0;
            } else {
                // 驱逐：保存被驱逐键与其 payload，当前键与 payload 占据此槽位
                // 读出被驱逐键
                std::string evicted_key(slot.key, strnlen(slot.key, MAX_KEY_SIZE));
                // 读出被驱逐 payload
                std::vector<char> evicted_payload(VALUE_SLOT_SIZE);
                std::memcpy(evicted_payload.data(), slot_base, VALUE_SLOT_SIZE);

                // 写入当前键与 payload 覆盖该槽位（新值替代原本 value 存储位置）
                std::memset(slot.key, 0, MAX_KEY_SIZE);
                std::memcpy(slot.key, cur_key.data(), cur_key_size);
                slot.valid = true;
                slot.value_offset = off;
                std::memcpy(slot_base, cur_payload.data(), VALUE_SLOT_SIZE);

                // 被驱逐项携带其原 payload 继续找位
                cur_key.swap(evicted_key);
                cur_payload.swap(evicted_payload);
                cur_key_size = std::min(cur_key.size(), (size_t)MAX_KEY_SIZE);
            }
        }
    }
    // 超过循环次数仍未安置成功
    return -1;
}

void CuckooHash::parse_values_fixed(const std::vector<char>& data, std::vector<KVPair>& out) {
    out.clear();
    size_t offset = 0;
    for (size_t i = 0; i < MAX_FIELDS; ++i) {
        if (offset + PER_PAIR_BYTES > data.size()) break;
        const char* pair_base = data.data() + offset;
        // field
        size_t f_len = strnlen(pair_base, FIELD_CAP);
        std::string field(pair_base, f_len);
        // value
        size_t v_len = strnlen(pair_base + FIELD_CAP, VALUE_CAP);
        std::string value(pair_base + FIELD_CAP, v_len);
        if (!field.empty()) {
            out.emplace_back(std::move(field), std::move(value));
        }
        offset += PER_PAIR_BYTES;
    }
}

int CuckooHash::Read(const std::string &key,std::vector<KVPair> &result) {
    // 1. 计算哈希值并找到对应的槽位
    // std::cout << "CuckooHash::Read: key=" << key << std::endl;
    for (size_t t = 0; t < hash_function_num; ++t) {
        size_t idx = hash_functions[t](key.c_str(), key.size()) % table_size;
        auto& slot = KVHashTable_vec[t][idx];
        // std::cout << "  Checking hash function " << t << ", index " << idx << " slot key: " << std::string(slot.key, slot.key_size)<< " slot key size: " << slot.key_size << std::endl;
        // 2. 检查槽位是否有效
        if (slot.valid && key == std::string(slot.key, key.size())) {
            // 3. 读取槽位中的值
            const char* value_base = value_storage[t] + idx * VALUE_SLOT_SIZE;
            std::vector<char> value_data(VALUE_SLOT_SIZE);
            std::memcpy(value_data.data(), value_base, VALUE_SLOT_SIZE);

            // 4. 解析值并填充结果
            parse_values_fixed(value_data, result);
            // std::cout << "Found key: " << key << " with " << result.size() << " values." << std::endl;
            // std::cout << "find key at table " << t << ", index " << idx << std::endl;
            return 0;
        }
    }
    // 未找到对应的键
    return -1;
}

size_t CuckooHash::get_value_slot_size() {
    return VALUE_SLOT_SIZE;
}