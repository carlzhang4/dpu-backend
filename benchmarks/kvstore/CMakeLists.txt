cmake_minimum_required(VERSION 3.0)




find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Development REQUIRED)

set(VC_INCLUDE_DIRECTORIES ../../verbose/src)
set(UFI_INCLUDE_DIRECTORIES ../../ufi/src ../../ufi/include)
set(COMMONS_INCLUDE_DIRECTORIES ../../commons/include ../../commons/src ../../commons/src/properties)
set(INCLUDE_DIRECTORIES ../../api/include/lowlevel ../../api/include/api ../../api/src/include ../../api/src/api/include ${UFI_INCLUDE_DIRECTORIES} ${COMMONS_INCLUDE_DIRECTORIES} ${VC_INCLUDE_DIRECTORIES} ${LIBELF_INCLUDE_DIR} ${Python3_INCLUDE_DIRS} ${LIBNUMA_INCLUDE_DIR} ${GEN_INCLUDE_DIRECTORY})

set(PROPERTIES_SOURCES
        ../../commons/src/properties/dpu_properties.c
        ../../commons/src/properties/dpu_properties_loader.c
        )

set(DESCRIPTION_SOURCES
        ../../commons/src/types/dpu_description.c
        )

set(COMMONS_SOURCES
        ${PROPERTIES_SOURCES}
        ${DESCRIPTION_SOURCES}
        ../../commons/src/dpu_predef_programs.c
        ../../commons/src/dpu_package.c
        )


        set(SOURCES
        ../../api/src/api/dpu_thread_job.c
        ../../api/src/api/dpu_polling.c
        ../../api/src/api/dpu_loader.c
        ../../api/src/api/dpu_runner.c
        ../../api/src/api/dpu_set.c
        ../../api/src/api/dpu_memory.c
        ../../api/src/api/dpu_checkpoint.c
        ../../api/src/api/dpu_python_wrappers.c
        ../../api/src/dpu_custom.c
        ../../api/src/dpu_log.c
        ../../api/src/dpu_elf.c
        ../../api/src/dpu_error.c
        ../../api/src/dpu_config.c
        ../../api/src/dpu_debug.c
        ../../api/src/dpu_internals.c
        ../../api/src/dpu_loader.c
        ../../api/src/dpu_profiler.c
        ../../api/src/dpu_program.c
        ../../api/src/dpu_management.c
        ../../api/src/dpu_memory.c
        ../../api/src/dpu_rank_handler_allocator.c
        ../../api/src/dpu_runner.c
        ../../api/src/runtime_info.c

        ../../ufi/src/ufi.c
        ../../ufi/src/ufi_bit_config.c
        ../../ufi/src/ufi_dma_wavegen_config.c
        ../../ufi/src/ufi_ci.c
        ../../ufi/src/ufi_config.c
        ../../ufi/src/ufi_debug.c
        ../../ufi/src/ufi_memory.c
        ../../ufi/src/ufi_runner.c
        )

set(ALL_DPU_SOURCES ${SOURCES} ${COMMONS_SOURCES})

if(ENABLE_DEBUG)
	message(STATUS "Add -g for /example")
	add_definitions("-g")
endif()

add_definitions("-D UINT32")


set(SRCS
        ${CMAKE_SOURCE_DIR}/libr_test/src/connection_manager.cpp
        ${CMAKE_SOURCE_DIR}/libr_test/src/libr.cpp
        ${CMAKE_SOURCE_DIR}/libr_test/src/util.cpp
		# ${CMAKE_SOURCE_DIR}/src/devx_device.cpp
	${CMAKE_SOURCE_DIR}/libr_test/src/devx_mr.cpp
    ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/src/kvstore.cpp
    ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/src/CuckooHash.cpp
)



include_directories("${CMAKE_SOURCE_DIR}/libr_test/include")
include_directories("${CMAKE_SOURCE_DIR}/libr_test/include/devx")
include_directories("${CMAKE_SOURCE_DIR}/libr_test/include/fw")
include_directories("${CMAKE_SOURCE_DIR}/benchmarks/kvstore/include")
include_directories("${CMAKE_SOURCE_DIR}/pimnic")


set(LIBRARIES ${LIBRARIES} ibverbs pthread numa gflags  mlx5)



add_executable(kvstore_test  ./kvstore_test.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_test PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_test  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_test PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_test PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_test gen_files)

# add_executable(kvstore_cpu  ./kvstore_cpu.cpp ${ALL_DPU_SOURCES} ${SRCS})  
# target_include_directories(kvstore_cpu PUBLIC ${INCLUDE_DIRECTORIES} )     
# target_link_libraries(kvstore_cpu  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
# target_compile_definitions(kvstore_cpu PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
# set_target_properties(kvstore_cpu PROPERTIES VERSION ${UPMEM_VERSION})
# add_dependencies(kvstore_cpu gen_files)

add_executable(kvstore_pim  ./kvstore_pim.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_pim PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_pim  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_pim PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_pim PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_pim gen_files)

add_executable(kvstore_ycsbc_cpu  ./kvstore_ycsbc_cpu.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_ycsbc_cpu PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_ycsbc_cpu  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_ycsbc_cpu PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_ycsbc_cpu PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_ycsbc_cpu gen_files)

add_executable(kvstore_ycsbc_pim  ./kvstore_ycsbc_pim.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_ycsbc_pim PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_ycsbc_pim  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_ycsbc_pim PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_ycsbc_pim PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_ycsbc_pim gen_files)

add_executable(kvstore_ycsbc_pim_multithread  ./kvstore_ycsbc_pim_multithread.cpp ${ALL_DPU_SOURCES} ${SRCS})
target_include_directories(kvstore_ycsbc_pim_multithread PUBLIC ${INCLUDE_DIRECTORIES} )
target_link_libraries(kvstore_ycsbc_pim_multithread  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_ycsbc_pim_multithread PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_ycsbc_pim_multithread PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_ycsbc_pim_multithread gen_files)

add_executable(kvstore_ycsbc_cpu_multithread  ./kvstore_ycsbc_cpu_multithread.cpp ${ALL_DPU_SOURCES} ${SRCS})
target_include_directories(kvstore_ycsbc_cpu_multithread PUBLIC ${INCLUDE_DIRECTORIES} )
target_link_libraries(kvstore_ycsbc_cpu_multithread  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_ycsbc_cpu_multithread PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_ycsbc_cpu_multithread PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_ycsbc_cpu_multithread gen_files)

add_executable(kvstore_pim_multitasklets  ./kvstore_pim_multitasklets.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_pim_multitasklets PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_pim_multitasklets  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_pim_multitasklets PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_pim_multitasklets PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_pim_multitasklets gen_files)

add_executable(kvstore_pim_multidpu  ./kvstore_pim_multidpu.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_pim_multidpu PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_pim_multidpu  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_pim_multidpu PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_pim_multidpu PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_pim_multidpu gen_files)

add_executable(kvstore_pim_cuckoo  ./kvstore_pim_cuckoo.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_pim_cuckoo PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_pim_cuckoo  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_pim_cuckoo PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_pim_cuckoo PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_pim_cuckoo gen_files)

add_executable(cuckoo_test  ./cuckoo_test.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(cuckoo_test PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(cuckoo_test  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(cuckoo_test PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(cuckoo_test PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(cuckoo_test gen_files)

add_executable(kvstore_pimnic_host  ./kvstore_pimnic_host.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_pimnic_host PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_pimnic_host  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_pimnic_host PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_pimnic_host PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_pimnic_host gen_files)



add_executable(kvstore_pimnic_host_varidpu  ./kvstore_pimnic_host_varidpu.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_pimnic_host_varidpu PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_pimnic_host_varidpu  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_pimnic_host_varidpu PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_pimnic_host_varidpu PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_pimnic_host_varidpu gen_files)

add_executable(kvstore_pimnic_ycsb  ./kvstore_pimnic_ycsb.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_pimnic_ycsb PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_pimnic_ycsb  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_pimnic_ycsb PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_pimnic_ycsb PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_pimnic_ycsb gen_files)

add_executable(kvstore_pimnic_ycsb_mt  ./kvstore_pimnic_ycsb_mt.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_pimnic_ycsb_mt PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_pimnic_ycsb_mt  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_pimnic_ycsb_mt PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_pimnic_ycsb_mt PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_pimnic_ycsb_mt gen_files)

add_executable(kvstore_cpu  ./kvstore_cpu.cpp  ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/include/SipHash.h ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/include/xxHash64.h ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_cpu PUBLIC ${INCLUDE_DIRECTORIES}  ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/include)     
target_link_libraries(kvstore_cpu  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_cpu PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_cpu PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_cpu gen_files)


#Device target
set(INPUT_kvstore_device "${CMAKE_CURRENT_SOURCE_DIR}/kvstore_device.c")
set(OUTPUT_kvstore_device "kvstore_device")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_device}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=16 -DBL=10 ${INPUT_kvstore_device} -o ${OUTPUT_kvstore_device}
  DEPENDS ${INPUT_kvstore_device}
  COMMENT "Generating ${OUTPUT_kvstore_device}"
)

set(INPUT_kvstore_kventry_processor "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_kventry_processor.c")
set(OUTPUT_kvstore_kventry_processor "kvstore_kventry_processor")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_kventry_processor}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=16 -DBL=10 ${INPUT_kvstore_kventry_processor} -o ${OUTPUT_kvstore_kventry_processor}
  DEPENDS ${INPUT_kvstore_kventry_processor} ${CMAKE_CURRENT_SOURCE_DIR}/include/KVStore_Config.h
  COMMENT "Generating ${OUTPUT_kvstore_kventry_processor}"
)

set(INPUT_kvstore_get_device "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_get_device.c")
set(OUTPUT_kvstore_get_device "kvstore_get_device")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_get_device}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DNR_TASKLETS=1 -DBL=10 ${INPUT_kvstore_get_device} -o ${OUTPUT_kvstore_get_device}
  DEPENDS ${INPUT_kvstore_get_device}
  COMMENT "Generating ${OUTPUT_kvstore_get_device}"
)

# set(INPUT_kvstore_get_device_tasklets_parallel "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_get_device_tasklets_parallel.c")
# set(OUTPUT_kvstore_get_device_tasklets_parallel "kvstore_get_device_tasklets_parallel")

# add_custom_command(
#   OUTPUT ${OUTPUT_kvstore_get_device_tasklets_parallel}
#   COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DNR_TASKLETS=1 -DBL=10 ${INPUT_kvstore_get_device_tasklets_parallel} -o ${OUTPUT_kvstore_get_device_tasklets_parallel}
#   DEPENDS ${INPUT_kvstore_get_device_tasklets_parallel}
#   COMMENT "Generating ${OUTPUT_kvstore_get_device_tasklets_parallel}"
# )

set(INPUT_kvstore_get_device_multitasklets "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_get_device_multitasklets.c")
set(OUTPUT_kvstore_get_device_multitasklets "kvstore_get_device_multitasklets")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_get_device_multitasklets}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DNR_TASKLETS=3 -DBL=10 ${INPUT_kvstore_get_device_multitasklets} -o ${OUTPUT_kvstore_get_device_multitasklets}
  DEPENDS ${INPUT_kvstore_get_device_multitasklets}
  COMMENT "Generating ${OUTPUT_kvstore_get_device_multitasklets}"
)

set(INPUT_kvstore_get_device_hash2 "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_get_device_hash2.c")
set(OUTPUT_kvstore_get_device_hash2 "kvstore_get_device_hash2")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_get_device_hash2}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=1 -DBL=10 ${INPUT_kvstore_get_device_hash2} -o ${OUTPUT_kvstore_get_device_hash2}
  DEPENDS ${INPUT_kvstore_get_device_hash2}
  COMMENT "Generating ${OUTPUT_kvstore_get_device_hash2}"
)

set(INPUT_kvstore_get_device_hash3 "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_get_device_hash3.c")
set(OUTPUT_kvstore_get_device_hash3 "kvstore_get_device_hash3")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_get_device_hash3}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=1 -DBL=10 ${INPUT_kvstore_get_device_hash3} -o ${OUTPUT_kvstore_get_device_hash3}
  DEPENDS ${INPUT_kvstore_get_device_hash3}
  COMMENT "Generating ${OUTPUT_kvstore_get_device_hash3}"
)

set(INPUT_kvstore_get_device_profile "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_get_device_profile.c")
set(OUTPUT_kvstore_get_device_profile "kvstore_get_device_profile")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_get_device_profile}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -pg -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DNR_TASKLETS=1 -DBL=10 ${INPUT_kvstore_get_device_profile} -o ${OUTPUT_kvstore_get_device_profile}
  DEPENDS ${INPUT_kvstore_get_device_profile}
  COMMENT "Generating ${OUTPUT_kvstore_get_device_profile}"
)

set(INPUT_kvstore_get_device_tasklets_parallel "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_get_device_tasklets_parallel.c")

foreach(COMBINED_VALUE RANGE 1 16)  # 遍历1到16（包含16）
    set(OUTPUT_NAME "kvstore_get_device_tasklets_parallel_${COMBINED_VALUE}")  # 单参数后缀
    add_custom_command(
        OUTPUT ${OUTPUT_NAME}
        COMMAND dpu-upmem-dpurte-clang 
            -Wall -Wextra -g 
            -I${CMAKE_CURRENT_SOURCE_DIR}/include 
            -Isupport 
            -O2 
            -DNR_TASKLETS=${COMBINED_VALUE}     # 设置相等的NR_TASKLETS
            -DTOTAL_TASKLETS=${COMBINED_VALUE}  # 设置相等的TOTAL_TASKLETS
            -DBL=10 
            ${INPUT_kvstore_get_device_tasklets_parallel} 
            -o ${OUTPUT_NAME}
        DEPENDS ${INPUT_kvstore_get_device_tasklets_parallel}
        COMMENT "Building ${OUTPUT_NAME} with NR_TASKLETS=TOTAL_TASKLETS=${COMBINED_VALUE}"
    )
    list(APPEND ALL_OUTPUTS ${OUTPUT_NAME})
endforeach()

set(INPUT_kvstore_pimnic "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_pimnic.c")
set(OUTPUT_kvstore_pimnic "kvstore_pimnic")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_pimnic}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -pg -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DNR_TASKLETS=1 -DBL=10 ${INPUT_kvstore_pimnic} -o ${OUTPUT_kvstore_pimnic}
  DEPENDS ${INPUT_kvstore_pimnic}
  COMMENT "Generating ${OUTPUT_kvstore_pimnic}"
)

set(INPUT_kvstore_pim_ycsb "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_pim_ycsb.c")
set(OUTPUT_kvstore_pim_ycsb "kvstore_pim_ycsb")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_pim_ycsb}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -pg -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DNR_TASKLETS=1 -DBL=10 ${INPUT_kvstore_pim_ycsb} -o ${OUTPUT_kvstore_pim_ycsb}
  DEPENDS ${INPUT_kvstore_pim_ycsb}
  COMMENT "Generating ${OUTPUT_kvstore_pim_ycsb}"
)

set(INPUT_kvstore_pim_ycsb_batching "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_pim_ycsb_batching.c")
set(OUTPUT_kvstore_pim_ycsb_batching "kvstore_pim_ycsb_batching")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_pim_ycsb_batching}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -pg -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DNR_TASKLETS=16 -DBL=10 ${INPUT_kvstore_pim_ycsb_batching} -o ${OUTPUT_kvstore_pim_ycsb_batching}
  DEPENDS ${INPUT_kvstore_pim_ycsb_batching}
  COMMENT "Generating ${OUTPUT_kvstore_pim_ycsb_batching}"
)

add_custom_target(
  generate_output_files_kvstore
  ALL
  DEPENDS ${OUTPUT_kvstore_device}
  DEPENDS ${OUTPUT_kvstore_kventry_processor}
  DEPENDS ${OUTPUT_kvstore_get_device}
  DEPENDS ${OUTPUT_kvstore_get_device_multitasklets}
  DEPENDS ${OUTPUT_kvstore_get_device_hash2}
  DEPENDS ${OUTPUT_kvstore_get_device_hash3}
  DEPENDS ${OUTPUT_kvstore_get_device_profile}
  DEPENDS ${OUTPUT_kvstore_get_device_tasklets_parallel}
  DEPENDS ${OUTPUT_kvstore_pimnic}
  DEPENDS ${OUTPUT_kvstore_pim_ycsb}
  DEPENDS ${OUTPUT_kvstore_pim_ycsb_batching}
  DEPENDS ${ALL_OUTPUTS}
)

add_subdirectory(YCSB-C)