set(SRCS
        ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/YCSB-C/core/core_workload.cc
        ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/YCSB-C/db/db_factory.cc
        ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/YCSB-C/db/PIM_db.cc
        ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/YCSB-C/db/hashtable_db.cc
        ${CMAKE_SOURCE_DIR}/libr_test/src/connection_manager.cpp
        ${CMAKE_SOURCE_DIR}/libr_test/src/libr.cpp
        ${CMAKE_SOURCE_DIR}/libr_test/src/util.cpp
	    ${CMAKE_SOURCE_DIR}/libr_test/src/devx_mr.cpp
)

include_directories("${CMAKE_SOURCE_DIR}/libr_test/include")
include_directories("${CMAKE_SOURCE_DIR}/libr_test/include/devx")
include_directories("${CMAKE_SOURCE_DIR}/libr_test/include/fw")

# Threads support (required for std::thread on Linux)
find_package(Threads REQUIRED)

# target_include_directories(YCSB-C
#         ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/YCSB-C/lib
#         ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/YCSB-C/core
#         ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/YCSB-C/db
# )

add_executable(ycsbc
        ./ycsbc.cc
        ${SRCS}
)

# Use the YCSB-C root so includes like "db/xyz.h" and "core/xyz.h" resolve
target_include_directories(ycsbc PUBLIC
        ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/YCSB-C
)

set(LIBRARIES ${LIBRARIES} ibverbs pthread numa gflags  mlx5)

target_link_libraries(ycsbc PRIVATE  ${LIBRARIES})

add_executable(ycsbc_batching ./ycsbc_batching ${SRCS})

target_link_libraries(ycsbc_batching PRIVATE  ${LIBRARIES})
target_include_directories(ycsbc_batching PUBLIC
        ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/YCSB-C
)