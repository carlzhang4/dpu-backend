//
//  basic_db.cc
//  YCSB-C
//
//  Created by Jinglei Ren on 12/17/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#include "db/db_factory.h"

#include <string>
#include "db/basic_db.h"
// #include "db/lock_stl_db.h"
// #include "db/redis_db.h"
// #include "db/tbb_rand_db.h"
// #include "db/tbb_scan_db.h"
#include "db/PIM_db.h"

using namespace std;
using ycsbc::DB;
using ycsbc::DBFactory;

DB* DBFactory::CreateDB(utils::Properties &props,NetParam &net_param) {
  if (props["dbname"] == "basic") {
    return new BasicDB;
  } else if (props["dbname"] == "PIM") {

    return new PIMDB(net_param);
  }else{
    return NULL;
  }
   
}

