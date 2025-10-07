//
//  PIM_db.h
//  YCSB-C
//

#ifndef YCSB_C_PIM_DB_H_
#define YCSB_C_PIM_DB_H_

#include "core/db.h"

#include <iostream>
#include <string>
#include <vector>
#include "core/properties.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <x86intrin.h>
#include <immintrin.h>
#include <sys/sysinfo.h>
#include <getopt.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>  // For mkdir

#include <sys/socket.h>
#include <netinet/in.h>
#include "libr.hpp"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

using std::cout;
using std::endl;



namespace ycsbc {

class PIMDB : public DB {
 public:

  struct kv_update_request {
      size_t total_size;
      size_t key_size;
      std::string key;
      std::vector<KVPair> values;
      
  };
  PIMDB(NetParam &net_param);

  int Read(const std::string &table, const std::string &key,
           const std::vector<std::string> *fields,
           std::vector<KVPair> &result);

  int Scan(const std::string &table, const std::string &key,
           int len, const std::vector<std::string> *fields,
           std::vector<std::vector<KVPair>> &result) {
    throw "Scan: function not implemented!";
  }

  int Update(const std::string &table, const std::string &key,
             std::vector<KVPair> &values) ;

  int Insert(const std::string &table, const std::string &key,
             std::vector<KVPair> &values) {
    std::cout << "PIMDB::Insert: key=" << key << " values.size=" << values.size() << std::endl;
    return Update(table, key, values);
  }

  int Delete(const std::string &table, const std::string &key) {
    std::string cmd("DEL " + key);
    
    return DB::kOK;
  }

  private:
    NetParam net_param_;
    
    static const int BUF_SIZE = 1UL*1024*1024*1024; // 1GB
    uint64_t magic_num ;
    uint64_t buffer_offset;
    QpHandler *qp_handlers;
    void * buf;
    struct ibv_wc *wc_send;
};



} // namespace ycsbc

#endif // YCSB_C_PIM_DB_H_

