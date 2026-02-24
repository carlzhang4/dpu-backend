#include "db/PIM_db.h"

#include <cstring>

using namespace std;

namespace ycsbc {

PIMDB::PIMDB(NetParam &net_param)
{
  net_param_ = net_param;
  buf = malloc_2m_numa(BUF_SIZE, net_param.numa_node);
  PingPongInfo *info = new PingPongInfo[net_param.numNodes]();
  
  memset(buf, 0, BUF_SIZE);
  qp_handlers = create_qp_rc(net_param, buf, BUF_SIZE, info , 0);
  exchange_data(net_param, reinterpret_cast<char *>(info), sizeof(PingPongInfo) );
  int my_id = net_param.nodeId;
	int dest_id = (net_param.nodeId + 1) % net_param.numNodes;
	for (int i = 0;i < 1;i++) {
		connect_qp_rc(net_param, *qp_handlers, info + dest_id * 1 + i, info + my_id * 1 + i);
	}
  magic_num = 1;
  buffer_offset = 0;
  
  ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
//   //* wating for server to be ready
//   char start_buf[10];
// 	int bytes_recv = recv(net_param.sockfd[0], start_buf, sizeof(start_buf), 0);
	
    std::cout << " received ready signal from server" << std::endl;

}

/*
request format:
| key size | key | magic_num |
| 8 bytes | key_size | 8 bytes |

response format:
| value size | value | magic_num |
| 8 bytes | value_size | 8 bytes |
*/

    int PIMDB::Read(const std::string &table, const std::string &key,
                     const std::vector<std::string> *fields,
                     std::vector<KVPair> &result) {
        
        
        // request format: [ key_size: u64 ][ key bytes ][ magic_num: u64 ]
        const uint64_t key_len = static_cast<uint64_t>(key.size());
        const size_t req_size = sizeof(uint64_t) + static_cast<size_t>(key_len) + sizeof(uint64_t);
        auto align64 = [](size_t x) { return (x + 63) & ~size_t(63); };

        // Wrap buffer if needed to avoid overflow
        if (buffer_offset + req_size > BUF_SIZE) {
            buffer_offset = 0;
        }

        const size_t req_offset = buffer_offset;
        uint8_t *req_base = reinterpret_cast<uint8_t *>(buf) + req_offset;

        // Compose request into registered buffer
        std::memcpy(req_base, &key_len, sizeof(uint64_t));
        std::memcpy(req_base + sizeof(uint64_t), key.data(), key.size());
        std::memcpy(req_base + sizeof(uint64_t) + key.size(), &magic_num, sizeof(uint64_t));

        // RDMA write request to server
        std::cout << "post send key to server" << std::endl;
        post_send(*qp_handlers, req_offset, req_size);
        std::cout << "post send key to server" << std::endl;

        while (!poll_send_cq(*qp_handlers, wc_send)) {}

        std::cout << "poll send key to server";
        // Response starts right after the request (aligned)
        const size_t resp_offset = req_offset + align64(req_size);
        volatile uint8_t *resp_base = reinterpret_cast<volatile uint8_t *>(buf) + resp_offset;

        // Poll: when trailing 8 bytes equals magic_num, response is complete.
        // Layout: [ value_size: u64 ][ value bytes ][ magic: u64 ]
        // We first wait for a plausible value_size, then check the magic at tail.
        uint64_t value_size = 0;
        for (;;) {
            value_size = *reinterpret_cast<volatile const uint64_t *>(resp_base);
            if (value_size > 0 && resp_offset + 8 + value_size + 8 <= BUF_SIZE) {
                volatile const uint64_t *tail_ptr = reinterpret_cast<volatile const uint64_t *>(
                    resp_base + 8 + value_size);
                if (*tail_ptr == magic_num) {
                    break;
                }
            }
            // optional: cpu_relax or small pause could be added here
        }
        result.clear();
        // Parse value into result vector
        // Here we assume value is a series of KVPair serialized as:
        // [ num_pairs: u64 ]
        //   repeat num_pairs times:
        //     [ field_len: u64 ][ field bytes ][ value_len: u64 ][ value bytes ]
        const uint8_t *value_ptr = (uint8_t *)buf + resp_offset + 8;
        uint64_t num_pairs = *reinterpret_cast<const uint64_t *>(value_ptr);
        value_ptr += sizeof(uint64_t);
        for (uint64_t i = 0; i < num_pairs; ++i) {
            uint64_t field_len = *reinterpret_cast<const uint64_t *>(value_ptr);
            value_ptr += sizeof(uint64_t);
            std::string field(reinterpret_cast<const char *>(value_ptr), field_len);
            value_ptr += field_len;
            uint64_t val_len = *reinterpret_cast<const uint64_t *>(value_ptr);
            value_ptr += sizeof(uint64_t);
            std::string val(reinterpret_cast<const char *>(value_ptr), val_len);
            value_ptr += val_len;
            result.emplace_back(std::move(field), std::move(val));
        }
        std::cout << "Received value of size: " << value_size << std::endl;
        // for(const auto& kv : result) {
        //     std::cout << "  field=" << kv.first << " value=" << kv.second << std::endl;
        // }
        // Optionally, copy the value into result if needed by YCSB
        // std::string value(reinterpret_cast<const char *>(resp_base + 8), value_size);
        // result.emplace_back("value", std::move(value));

        // Advance buffer_offset to after the response (aligned) and bump magic
        buffer_offset = resp_offset + align64(8 + static_cast<size_t>(value_size) + 8);
        magic_num++;
        return DB::kOK;
    }


  /*
  
  size_t total_size = 
   2 * sizeof(size_t) +              // total_size 和 key_size 字段
   key.size() +                      // key 内容
   sizeof(size_t) +                  // values 的数量字段
   values.size() * 2 * sizeof(size_t) +  // 所有 KVPair 的 key/value 长度字段
   sum_of_all_key_sizes +            // 所有 KVPair 的 key 内容总和
   sum_of_all_value_sizes;           // 所有 KVPair 的 value 内容总和
   
   */


static inline void append_u64_be(std::vector<char>& out, uint64_t v) {
    uint64_t be = htobe64(v);
    const char* p = reinterpret_cast<const char*>(&be);
    out.insert(out.end(), p, p + sizeof(be));
}


  int PIMDB::Read_Batching(const std::string &table,
                              const std::vector<std::string> &keys,
                              const std::vector<std::string> *fields,
                              std::vector<std::vector<KVPair>> &results){
    // Batch request/response (single-shot):
    // Request layout (contiguous in registered buf):
    //   [ num_keys: u64 ]
    //   repeat num_keys times: [ key_len: u64 ][ key bytes ]
    //   [ magic: u64 ]
    // Response layout:
    //   [ resp_size: u64 ]  -- size of payload bytes that follow (not including this header or trailing magic)
    //   [ payload bytes ]   -- payload format below
    //   [ magic: u64 ]
    // Payload format:
    //   [ num_keys: u64 ]
    //     for each key i in [0..num_keys):
    //       [ num_pairs: u64 ]
    //         repeat num_pairs times:
    //           [ field_len: u64 ][ field bytes ][ value_len: u64 ][ value bytes ]

    auto align64 = [](size_t x) { return (x + 63) & ~size_t(63); };

    // Compute total request size
    const uint64_t num_keys = static_cast<uint64_t>(keys.size());
    size_t req_size = sizeof(uint64_t); // num_keys
    for (const auto &k : keys) {
        req_size += sizeof(uint64_t) + k.size();
    }
    req_size += sizeof(uint64_t); // magic

    // Wrap if needed
    if (buffer_offset + req_size > BUF_SIZE) {
        buffer_offset = 0;
    }

    const size_t req_offset = buffer_offset;
    uint8_t *base = reinterpret_cast<uint8_t *>(buf);
    uint8_t *req_ptr = base + req_offset;

    // Serialize request
    std::memcpy(req_ptr, &num_keys, sizeof(uint64_t));
    req_ptr += sizeof(uint64_t);
    for (const auto &k : keys) {
        const uint64_t klen = static_cast<uint64_t>(k.size());
        std::memcpy(req_ptr, &klen, sizeof(uint64_t));
        req_ptr += sizeof(uint64_t);
        if (klen) {
            std::memcpy(req_ptr, k.data(), k.size());
            req_ptr += k.size();
        }
    }
    std::memcpy(req_ptr, &magic_num, sizeof(uint64_t));

    // Issue a single RDMA write for the whole batch
    post_send(*qp_handlers, req_offset, static_cast<int>(req_size));
    while (!poll_send_cq(*qp_handlers, wc_send)) {}

    // Response will start at next 64B boundary
    const size_t resp_offset = req_offset + align64(req_size);
    volatile uint8_t *resp_base = reinterpret_cast<volatile uint8_t *>(buf) + resp_offset;

    // Spin until tail magic matches magic_num
    // First 8 bytes contain total payload size (resp_size)
    uint64_t resp_size = 0;
    for (;;) {
        resp_size = *reinterpret_cast<volatile const uint64_t *>(resp_base);
        if (resp_size > 0 && resp_offset + 8 + resp_size + 8 <= BUF_SIZE) {
            volatile const uint64_t *tail = reinterpret_cast<volatile const uint64_t *>(resp_base + 8 + resp_size);
            if (*tail == magic_num) {
                break;
            }
        }
        // busy wait
    }

    // Parse payload into results
    const uint8_t *p = reinterpret_cast<const uint8_t *>(buf) + resp_offset + 8; // skip resp_size
    const uint8_t *end = p + resp_size;

    results.clear();
    results.resize(keys.size());

    auto read_u64 = [&](const uint8_t *&ptr) -> uint64_t {
        uint64_t v = *reinterpret_cast<const uint64_t *>(ptr);
        ptr += sizeof(uint64_t);
        return v;
    };

    if (p + sizeof(uint64_t) > end) {
        // malformed response
        return DB::kErrorNoData;
    }
    uint64_t r_num_keys = read_u64(p);
    if (r_num_keys != num_keys) {
        // server returned a different count; treat as error
        return DB::kErrorNoData;
    }

    for (size_t i = 0; i < keys.size(); ++i) {
        if (p + sizeof(uint64_t) > end) return DB::kErrorNoData;
        uint64_t num_pairs = read_u64(p);
        auto &out = results[i];
        out.clear();
        out.reserve(static_cast<size_t>(num_pairs));
        for (uint64_t j = 0; j < num_pairs; ++j) {
            if (p + sizeof(uint64_t) > end) return DB::kErrorNoData;
            uint64_t field_len = read_u64(p);
            if (p + field_len + sizeof(uint64_t) > end) return DB::kErrorNoData;
            std::string field(reinterpret_cast<const char *>(p), static_cast<size_t>(field_len));
            p += field_len;
            uint64_t val_len = read_u64(p);
            if (p + val_len > end) return DB::kErrorNoData;
            std::string val(reinterpret_cast<const char *>(p), static_cast<size_t>(val_len));
            p += val_len;
            out.emplace_back(std::move(field), std::move(val));
        }
    }

    // Advance buffer pointer and bump magic
    buffer_offset = resp_offset + align64(8 + static_cast<size_t>(resp_size) + 8);
    magic_num++;
    return DB::kOK;
  }

static bool send_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 可根据需要添加重试/等待
                continue;
            }
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

int PIMDB::Update(const std::string &table, const std::string &key,
                  std::vector<KVPair> &values) {
    // 线协议：
    // [key_len: u64][key bytes][num_values: u64]
    //   repeat num_values times:
    //     [field_len: u64][field bytes][value_len: u64][value bytes]

                  
    // std::cout << "PIMDB::Update: key=" << key << " values.size=" << values.size() << std::endl;
    // for(const auto& kv : values) {
    //     std::cout << "  field=" << kv.first << " value size=" << kv.second.size() << std::endl;
    // }
    
    uint64_t num_values = static_cast<uint64_t>(values.size());

    size_t reserve_bytes = sizeof(uint64_t) + key.size() + sizeof(uint64_t);
    for (const auto& kv : values) {
        reserve_bytes += sizeof(uint64_t) + kv.first.size();   // field
        reserve_bytes += sizeof(uint64_t) + kv.second.size();  // value
    }

    std::vector<char> buf;
    buf.reserve(reserve_bytes);

    append_u64_be(buf, static_cast<uint64_t>(key.size()));
    buf.insert(buf.end(), key.begin(), key.end());

    append_u64_be(buf, num_values);
    for (const auto& kv : values) {
        append_u64_be(buf, static_cast<uint64_t>(kv.first.size()));
        buf.insert(buf.end(), kv.first.begin(), kv.first.end());

        append_u64_be(buf, static_cast<uint64_t>(kv.second.size()));
        buf.insert(buf.end(), kv.second.begin(), kv.second.end());
    }

    if (!send_all(net_param_.sockfd[0], buf.data(), buf.size())) {
        return DB::kErrorNoData;
    }
    return DB::kOK;
}
  

} // namespace ycsbc