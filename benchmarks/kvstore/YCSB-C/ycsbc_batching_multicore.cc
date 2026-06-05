//
//  ycsbc.cc
//  YCSB-C
//
//  Created by Jinglei Ren on 12/19/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#include <cstring>
#include <string>
#include <iostream>
#include <vector>
#include <future>
#include "core/utils.h"
#include "core/timer.h"
#include "core/client.h"
#include "core/core_workload.h"
#include "db/db_factory.h"
#include <gflags/gflags.h>
#include "libr.hpp"

using namespace std;


#define MAX_THREADS 48

void UsageMessage(const char *command);
bool StrStartWith(const char *str, const char *pre);
string ParseCommandLine(int argc, const char *argv[], utils::Properties &props);

int DelegateClient(ycsbc::DB *db, ycsbc::CoreWorkload *wl, const int num_ops,
    bool is_loading) {
  db->Init();
  ycsbc::Client client(*db, *wl);
  int oks = 0;
  for (int i = 0; i < num_ops; ++i) {
    if (is_loading) {
      oks += client.DoInsert();
    } else {
      oks += client.DoTransaction();
    }
  }
  db->Close();
  return oks;
}


int DelegateClient_batching(ycsbc::DB *db, ycsbc::CoreWorkload *wl, const int num_ops,
    bool is_loading) {
  db->Init();
  ycsbc::Client client(*db, *wl);
  int oks = 0;
  int batch_size = 64; 
  numa_run_on_node(1);   // 限制线程只在该 NUMA node 上运行
  numa_set_preferred(1); // 内存分配也优先使用该节点
  //printf("Starting batching transactions...\n");
  for (int i = 0; i < num_ops;i+= batch_size) {
    //printf("Batching transaction %d\n", i);
      oks += client.TransactionRead_Batching();
  }
  db->Close();
  return oks;
}

// void thread_insert_and_get(int thread_index,NetParam net_param, utils::Properties prop)



int main(const int argc, const char *argv[]) {
  utils::Properties props;


  std::cout << "YCSB-C Benchmarking Tool" << std::endl;
  string file_name = ParseCommandLine(argc, argv, props);


  const int num_threads = stoi(props.GetProperty("threadcount", "1"));
  NetParam* net_param_ptr;
  net_param_ptr = new NetParam[MAX_THREADS];
  for(int i = 0; i < num_threads; i++){
    net_param_ptr[i].numNodes = 2;
    net_param_ptr[i].nodeId = 1;
    net_param_ptr[i].serverIp = "127.0.0.1";
    net_param_ptr[i].device_name = "mlx5_0";
    net_param_ptr[i].gid_index = 3;
    net_param_ptr[i].numa_node = 1;
    net_param_ptr[i].batch_size = 4096;
    net_param_ptr[i].sge_per_wr = 1;
    net_param_ptr[i].sock_port = 6666 + i;
    net_param_ptr[i].use_devx_context = false;
    init_net_param(net_param_ptr[i]);
    socket_init(net_param_ptr[i]);
    roce_init(net_param_ptr[i], 1);
  }

  std::cout << "Using database " << props["dbname"] << std::endl;
  // ycsbc::DB *db = ycsbc::DBFactory::CreateDB(props, net_param);
  ycsbc::DB *db[MAX_THREADS];
  for(int i = 0; i < num_threads; i++){
    std::cout << "starting to create database for thread " << i << std::endl;
    db[i] = ycsbc::DBFactory::CreateDB(props, net_param_ptr[i]);
    std::cout << "created database for thread " << i << std::endl;
  }

  std::cout << "Using workload " << props["workload"] << std::endl;
  if (!db) {
    cout << "Unknown database name " << props["dbname"] << endl;
    exit(0);
  }


  ycsbc::CoreWorkload wl;
  wl.Init(props);

  std::cout << "Using workload " << props["workload"] << std::endl;

  char start_buf[10];
  // 	int bytes_recv = recv(net_param.sockfd[0], start_buf, sizeof(start_buf), 0);
  for(int i = 0; i < num_threads; i++){
    int bytes_recv = recv(net_param_ptr[i].sockfd[0], start_buf, sizeof(start_buf), 0);
    std::cout << "received ready signal from server for thread " << i << std::endl;
  }   
  
  //getchar();
  // Loads data
  vector<future<int>> actual_ops;
  int total_ops = stoi(props[ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY]);
  for (int i = 0; i < num_threads; ++i) {
    actual_ops.emplace_back(async(launch::async,
        DelegateClient, db[i], &wl, 10000, true));
  }
  assert((int)actual_ops.size() == num_threads);

  int sum = 0;
  for (auto &n : actual_ops) {
    assert(n.valid());
    sum += n.get();
  }
  cerr << "# Loading records:\t" << sum << endl;
  getchar();
  // Peforms transactions
  actual_ops.clear();
  total_ops = stoi(props[ycsbc::CoreWorkload::OPERATION_COUNT_PROPERTY]);
  utils::Timer<double> timer;
  timer.Start();
  for (int i = 0; i < num_threads; ++i) {
    actual_ops.emplace_back(async(launch::async,
        DelegateClient_batching, db[i], &wl, total_ops / num_threads, false));
  }
  assert((int)actual_ops.size() == num_threads);

  sum = 0;
  for (auto &n : actual_ops) {
    assert(n.valid());
    sum += n.get();
  }
  double duration = timer.End();
  cerr << "# Transaction throughput (KTPS)" << endl;
  cerr << props["dbname"] << '\t' << file_name << '\t' << num_threads << '\t';
  cerr << total_ops / duration / 1000 << endl;
  std::cout << "total time : "<< duration*1000*1000 << " us" <<std::endl;
}


string ParseCommandLine(int argc, const char *argv[], utils::Properties &props) {
  int argindex = 1;
  string filename;
  while (argindex < argc && StrStartWith(argv[argindex], "-")) {
    if (strcmp(argv[argindex], "-threads") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("threadcount", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-db") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("dbname", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-host") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("host", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-port") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("port", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-slaves") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("slaves", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-P") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      filename.assign(argv[argindex]);
      ifstream input(argv[argindex]);
      try {
        props.Load(input);
      } catch (const string &message) {
        cout << message << endl;
        exit(0);
      }
      input.close();
      argindex++;
    } 
    else {
      cout << "Unknown option '" << argv[argindex] << "'" << endl;
      exit(0);
    }
  }

  if (argindex == 1 || argindex != argc) {
    UsageMessage(argv[0]);
    exit(0);
  }

  return filename;
}

void UsageMessage(const char *command) {
  cout << "Usage: " << command << " [options]" << endl;
  cout << "Options:" << endl;
  cout << "  -threads n: execute using n threads (default: 1)" << endl;
  cout << "  -db dbname: specify the name of the DB to use (default: basic)" << endl;
  cout << "  -P propertyfile: load properties from the given file. Multiple files can" << endl;
  cout << "                   be specified, and will be processed in the order specified" << endl;
}

inline bool StrStartWith(const char *str, const char *pre) {
  return strncmp(str, pre, strlen(pre)) == 0;
}

