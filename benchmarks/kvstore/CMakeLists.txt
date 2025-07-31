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
)



include_directories("${CMAKE_SOURCE_DIR}/libr_test/include")
include_directories("${CMAKE_SOURCE_DIR}/libr_test/include/devx")
include_directories("${CMAKE_SOURCE_DIR}/libr_test/include/fw")
include_directories("${CMAKE_SOURCE_DIR}/benchmarks/kvstore/include")


set(LIBRARIES ${LIBRARIES} ibverbs pthread numa gflags  mlx5)



add_executable(kvstore_test  ./kvstore_test.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_test PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_test  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_test PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_test PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_test gen_files)

add_executable(kvstore_cpu  ./kvstore_cpu.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_cpu PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_cpu  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_cpu PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_cpu PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_cpu gen_files)

add_executable(kvstore_pim  ./kvstore_pim.cpp ${ALL_DPU_SOURCES} ${SRCS})  
target_include_directories(kvstore_pim PUBLIC ${INCLUDE_DIRECTORIES} )     
target_link_libraries(kvstore_pim  dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES}  ${LIBRARIES} dpuverbose)
target_compile_definitions(kvstore_pim PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(kvstore_pim PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(kvstore_pim gen_files)

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
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=1 -DBL=10 ${INPUT_kvstore_get_device} -o ${OUTPUT_kvstore_get_device}
  DEPENDS ${INPUT_kvstore_get_device}
  COMMENT "Generating ${OUTPUT_kvstore_get_device}"
)

set(INPUT_kvstore_get_device_multitasklets "${CMAKE_CURRENT_SOURCE_DIR}/src/kvstore_get_device_multitasklets.c")
set(OUTPUT_kvstore_get_device_multitasklets "kvstore_get_device_multitasklets")

add_custom_command(
  OUTPUT ${OUTPUT_kvstore_get_device_multitasklets}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=3 -DBL=10 ${INPUT_kvstore_get_device_multitasklets} -o ${OUTPUT_kvstore_get_device_multitasklets}
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
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -pg -Isupport -O2 -DNR_TASKLETS=1 -DBL=10 ${INPUT_kvstore_get_device_profile} -o ${OUTPUT_kvstore_get_device_profile}
  DEPENDS ${INPUT_kvstore_get_device_profile}
  COMMENT "Generating ${OUTPUT_kvstore_get_device_profile}"
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
)