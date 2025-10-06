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
  //* wating for server to be ready
  char start_buf[10];
	int bytes_recv = recv(net_param.sockfd[0], start_buf, sizeof(start_buf), 0);
	

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
    std::cout << "PIMDB::Read: key=" << key << std::endl;
    uint64_t new_offset = buffer_offset;
    *(uint64_t*)((char*)buf + new_offset) = key.size();
    new_offset += sizeof(uint64_t);
    memcpy((char*)buf + new_offset, key.c_str(), key.size());
    new_offset += key.size();
    *(uint64_t*)((char*)buf + new_offset) = magic_num;
    new_offset += sizeof(uint64_t);


    post_send(*qp_handlers, buffer_offset, new_offset - buffer_offset);
    while(!poll_send_cq(*qp_handlers, wc_send));
    uint64_t value_size;
    std::cout << "PIMDB::Read: after post_send" << std::endl;
    buffer_offset = new_offset;
    //while(*(uint64_t*)((char*)buf + buffer_offset + *(uint64_t*)((char*)buf + buffer_offset)+ 8) != magic_num);
    return DB::kOK;
  }

  int PIMDB::Update(const std::string &table, const std::string &key,
             std::vector<KVPair> &values) {
    // std::cout << "PIMDB::Update: key=" << key << " key size=" << key.size() << " values.size=" << values.size() << std::endl;
    // std::cout << "KVPaoir size: " << values.size() << std::endl;
    // printf("PIMDB::Update: key=%s key size=%zu values.size=%zu\n", key.c_str(), key.size(), values.size());
    // for (const auto &pair : values) {
    //   printf("field %s: len %zu\n", pair.first.c_str(), pair.second.size());
    // }

    // return Update(table, key, values);
    return DB::kOK;
  }

  

} // namespace ycsbc