cmake_minimum_required(VERSION 3.0)



# add_compile_options(-g -o3 -fcoroutines -std=c++20 -static)

# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED True)
# set(CMAKE_EXPORT_COMPILE_COMMANDS ON)



# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -std=c++17 -Wall")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-old-style-cast -Wno-unused-function -Wno-missing-braces")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-address-of-packed-member -Wno-nested-anon-types -Wno-keyword-macro -Wno-deprecated-declarations")


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

# if(ENABLE_DEBUG)
# 	message(STATUS "Add -g for /example")
# 	add_definitions("-g")
# endif()

add_definitions("-D UINT32")


set(SRCS
        ${CMAKE_SOURCE_DIR}/libr_test/src/connection_manager.cpp
        ${CMAKE_SOURCE_DIR}/libr_test/src/libr.cpp
        ${CMAKE_SOURCE_DIR}/libr_test/src/util.cpp
		# ${CMAKE_SOURCE_DIR}/src/devx_device.cpp
	${CMAKE_SOURCE_DIR}/libr_test/src/devx_mr.cpp
)



include_directories("${CMAKE_SOURCE_DIR}/libr_test/include")
include_directories("${CMAKE_SOURCE_DIR}/libr_test/include/devx")
include_directories("${CMAKE_SOURCE_DIR}/libr_test/include/fw")


set(LIBRARIES ${LIBRARIES} ibverbs pthread numa gflags  mlx5)



add_executable(baseline_latency  ./baseline_latency.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(baseline_latency PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(baseline_latency  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(baseline_latency PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(baseline_latency PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(baseline_latency gen_files)

add_executable(baseline_throughput  ./baseline_throughput.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(baseline_throughput PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(baseline_throughput  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(baseline_throughput PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(baseline_throughput PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(baseline_throughput gen_files)

add_executable(idle_kernel  ./idle_kernel.c ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(idle_kernel PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(idle_kernel  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(idle_kernel PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(idle_kernel PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(idle_kernel gen_files)

#Device target
set(INPUT_baseline_latency_host "${CMAKE_CURRENT_SOURCE_DIR}/baseline_latency_host.c")
set(OUTPUT_baseline_latency_host "baseline_latency_host")

add_custom_command(
  OUTPUT ${OUTPUT_baseline_latency_host}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=16 -DBL=10 ${INPUT_baseline_latency_host} -o ${OUTPUT_baseline_latency_host}
  DEPENDS ${INPUT_baseline_latency_host}
  COMMENT "Generating ${OUTPUT_baseline_latency_host}"
)

set(INPUT_idle_kernel_host "${CMAKE_CURRENT_SOURCE_DIR}/idle_kernel_host.c")
set(OUTPUT_idle_kernel_host "idle_kernel_host")

add_custom_command(
  OUTPUT ${OUTPUT_idle_kernel_host}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=1 -DBL=10 ${INPUT_idle_kernel_host} -o ${OUTPUT_idle_kernel_host}
  DEPENDS ${INPUT_idle_kernel_host}
  COMMENT "Generating ${OUTPUT_idle_kernel_host}"
)

add_custom_target(
  generate_output_files_baseline_latency
  ALL
  DEPENDS ${OUTPUT_baseline_latency_host}
  DEPENDS ${OUTPUT_idle_kernel_host}
)