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
    # ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/src/kvstore.cpp
    # ${CMAKE_SOURCE_DIR}/benchmarks/kvstore/src/CuckooHash.cpp
)



include_directories("${CMAKE_SOURCE_DIR}/libr_test/include")
include_directories("${CMAKE_SOURCE_DIR}/libr_test/include/devx")
include_directories("${CMAKE_SOURCE_DIR}/libr_test/include/fw")
# include_directories("${CMAKE_SOURCE_DIR}/benchmarks/kvstore/include")
include_directories("${CMAKE_SOURCE_DIR}/pimnic")


set(LIBRARIES ${LIBRARIES} ibverbs pthread numa gflags  mlx5)

set(BL 10 CACHE STRING "Block length for select_pim")

add_executable(select_pim  ./select_pim.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(select_pim PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(select_pim  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(select_pim PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION} BL=${BL})
set_target_properties(select_pim PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(select_pim gen_files)


add_executable(select_pimnic_host_varidpu  ./select_pimnic_host_varidpu.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(select_pimnic_host_varidpu PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(select_pimnic_host_varidpu  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(select_pimnic_host_varidpu PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(select_pimnic_host_varidpu PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(select_pimnic_host_varidpu gen_files)


add_executable(select_cpu  ./select_cpu.cpp    ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(select_cpu PUBLIC ${INCLUDE_DIRECTORIES}  )     
target_link_libraries(select_cpu  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(select_cpu PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(select_cpu PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(select_cpu gen_files)

add_executable(select_cpu_bandwidth  ./select_cpu_bandwidth.cpp    ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(select_cpu_bandwidth PUBLIC ${INCLUDE_DIRECTORIES}  )     
target_link_libraries(select_cpu_bandwidth  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(select_cpu_bandwidth PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(select_cpu_bandwidth PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(select_cpu_bandwidth gen_files)

add_executable(select_pim_throughput  ./select_pim_throughput.cpp    ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(select_pim_throughput PUBLIC ${INCLUDE_DIRECTORIES}  )     
target_link_libraries(select_pim_throughput  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(select_pim_throughput PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(select_pim_throughput PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(select_pim_throughput gen_files)

#Device target
set(INPUT_select_device_tasklets_parallel "${CMAKE_CURRENT_SOURCE_DIR}/src/select_device_tasklets_parallel.c")

foreach(COMBINED_VALUE RANGE 1 16)  # 遍历1到16（包含16）
    set(OUTPUT_NAME "select_device_tasklets_parallel_${COMBINED_VALUE}")  # 单参数后缀
    add_custom_command(
        OUTPUT ${OUTPUT_NAME}
        COMMAND dpu-upmem-dpurte-clang 
            -Wall -Wextra -g 
            # -I${CMAKE_CURRENT_SOURCE_DIR}/include 
            -Isupport 
            -O2 
            -DNR_TASKLETS=${COMBINED_VALUE}     # 设置相等的NR_TASKLETS
            -DTOTAL_TASKLETS=${COMBINED_VALUE}  # 设置相等的TOTAL_TASKLETS
            -DBL=10 
            ${INPUT_select_device_tasklets_parallel} 
            -o ${OUTPUT_NAME}
        DEPENDS ${INPUT_select_device_tasklets_parallel}
        COMMENT "Building ${OUTPUT_NAME} with NR_TASKLETS=TOTAL_TASKLETS=${COMBINED_VALUE}"
    )
    list(APPEND ALL_OUTPUTS ${OUTPUT_NAME})
endforeach()

set(INPUT_SEL_throughput "${CMAKE_CURRENT_SOURCE_DIR}/src/select_device_throughput.c")
set(OUTPUT_SEL_throughput "select_device_throughput")

add_custom_command(
  OUTPUT ${OUTPUT_SEL_throughput}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -pg -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DINT8 -DNR_TASKLETS=16 -DBL=10 ${INPUT_SEL_throughput} -o ${OUTPUT_SEL_throughput}
  DEPENDS ${INPUT_SEL_throughput}
  COMMENT "Generating ${OUTPUT_SEL_throughput}"
)

add_custom_target(
  generate_output_files_sel
  ALL
  DEPENDS  ${OUTPUT_SEL_throughput}
  DEPENDS ${ALL_OUTPUTS}
)

