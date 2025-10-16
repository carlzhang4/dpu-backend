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

# set(BL 10 CACHE STRING "Block length for select_pim")

# add_executable(select_pim  ./select_pim.cpp ${ALL_DPU_SOURCES} ${SRCS})  
# target_include_directories(select_pim PUBLIC ${INCLUDE_DIRECTORIES} )     
# target_link_libraries(select_pim  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
# target_compile_definitions(select_pim PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION} BL=${BL})
# set_target_properties(select_pim PROPERTIES VERSION ${UPMEM_VERSION})
# add_dependencies(select_pim gen_files)



add_executable(GNN_host ./host/app.c ${ALL_DPU_SOURCES} ${SRCS})
target_include_directories(GNN_host PUBLIC ${INCLUDE_DIRECTORIES}   ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(GNN_host dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(GNN_host PRIVATE DPU_TOOLS_VERSION=${UPMEM_VERSION} NR_TASKLETS=16)
set_target_properties(GNN_host PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(GNN_host generate_output_files_GNN)

#Device target
# set(INPUT_select_device_tasklets_parallel "${CMAKE_CURRENT_SOURCE_DIR}/src/select_device_tasklets_parallel.c")

# foreach(COMBINED_VALUE RANGE 1 16)  # 遍历1到16（包含16）
#     set(OUTPUT_NAME "select_device_tasklets_parallel_${COMBINED_VALUE}")  # 单参数后缀
#     add_custom_command(
#         OUTPUT ${OUTPUT_NAME}
#         COMMAND dpu-upmem-dpurte-clang 
#             -Wall -Wextra -g 
#             # -I${CMAKE_CURRENT_SOURCE_DIR}/include 
#             -Isupport 
#             -O2 
#             -DNR_TASKLETS=${COMBINED_VALUE}     # 设置相等的NR_TASKLETS
#             -DTOTAL_TASKLETS=${COMBINED_VALUE}  # 设置相等的TOTAL_TASKLETS
#             -DBL=10 
#             ${INPUT_select_device_tasklets_parallel} 
#             -o ${OUTPUT_NAME}
#         DEPENDS ${INPUT_select_device_tasklets_parallel}
#         COMMENT "Building ${OUTPUT_NAME} with NR_TASKLETS=TOTAL_TASKLETS=${COMBINED_VALUE}"
#     )
#     list(APPEND ALL_OUTPUTS ${OUTPUT_NAME})
# endforeach()



# add_custom_target(
#   generate_output_files_sel
#   ALL
#   DEPENDS ${ALL_OUTPUTS}
# )

# DPU内核目标 - 使用自定义命令

set(INPUT_dpu_kernel_1 "${CMAKE_CURRENT_SOURCE_DIR}/src/GNN_kernel_1.c")
set(OUTPUT_dpu_kernel_1 "dpu_kernel_1")

add_custom_command(
  OUTPUT ${OUTPUT_dpu_kernel_1}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -pg -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DINT32 -DNR_TASKLETS=16 -DBL=10 ${INPUT_dpu_kernel_1} -o ${OUTPUT_dpu_kernel_1}
  DEPENDS ${INPUT_dpu_kernel_1}
  COMMENT "Generating ${OUTPUT_dpu_kernel_1}"
)


set(INPUT_dpu_kernel_2 "${CMAKE_CURRENT_SOURCE_DIR}/src/GNN_kernel_2.c")
set(OUTPUT_dpu_kernel_2 "dpu_kernel_2")

add_custom_command(
  OUTPUT ${OUTPUT_dpu_kernel_2}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -pg -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DINT32 -DNR_TASKLETS=16 -DBL=10 ${INPUT_dpu_kernel_2} -o ${OUTPUT_dpu_kernel_2}
  DEPENDS ${INPUT_dpu_kernel_2}
  COMMENT "Generating ${OUTPUT_dpu_kernel_2}"
)


set(INPUT_dpu_relocate_AG "${CMAKE_CURRENT_SOURCE_DIR}/src/dpu_relocate_AG.c")
set(OUTPUT_dpu_relocate_AG "dpu_relocate_AG")

add_custom_command(
  OUTPUT ${OUTPUT_dpu_relocate_AG}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -pg -I${CMAKE_CURRENT_SOURCE_DIR}/include -Isupport -O2 -DINT32 -DNR_TASKLETS=16 -DBL=10 ${INPUT_dpu_relocate_AG} -o ${OUTPUT_dpu_relocate_AG}
  DEPENDS ${INPUT_dpu_relocate_AG}
  COMMENT "Generating ${OUTPUT_dpu_relocate_AG}"
)


add_custom_target(
  generate_output_files_GNN
  ALL
  DEPENDS ${OUTPUT_dpu_kernel_1}
  DEPENDS ${OUTPUT_dpu_kernel_2}
  DEPENDS ${OUTPUT_dpu_relocate_AG}
)