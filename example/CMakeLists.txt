cmake_minimum_required(VERSION 3.13)

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Development REQUIRED)

set(VC_INCLUDE_DIRECTORIES ../verbose/src)
set(UFI_INCLUDE_DIRECTORIES ../ufi/src ../ufi/include)
set(COMMONS_INCLUDE_DIRECTORIES ../commons/include ../commons/src ../commons/src/properties)
set(INCLUDE_DIRECTORIES ../api/include/lowlevel ../api/include/api ../api/src/include ../api/src/api/include ${UFI_INCLUDE_DIRECTORIES} ${COMMONS_INCLUDE_DIRECTORIES} ${VC_INCLUDE_DIRECTORIES} ${LIBELF_INCLUDE_DIR} ${Python3_INCLUDE_DIRS} ${LIBNUMA_INCLUDE_DIR} ${GEN_INCLUDE_DIRECTORY})

set(PROPERTIES_SOURCES
        ../commons/src/properties/dpu_properties.c
        ../commons/src/properties/dpu_properties_loader.c
        )

set(DESCRIPTION_SOURCES
        ../commons/src/types/dpu_description.c
        )

set(COMMONS_SOURCES
        ${PROPERTIES_SOURCES}
        ${DESCRIPTION_SOURCES}
        ../commons/src/dpu_predef_programs.c
        ../commons/src/dpu_package.c
        )

set(SOURCES
        ../api/src/api/dpu_thread_job.c
        ../api/src/api/dpu_polling.c
        ../api/src/api/dpu_loader.c
        ../api/src/api/dpu_runner.c
        ../api/src/api/dpu_set.c
        ../api/src/api/dpu_memory.c
        ../api/src/api/dpu_checkpoint.c
        ../api/src/api/dpu_python_wrappers.c
        ../api/src/dpu_custom.c
        ../api/src/dpu_log.c
        ../api/src/dpu_elf.c
        ../api/src/dpu_error.c
        ../api/src/dpu_config.c
        ../api/src/dpu_debug.c
        ../api/src/dpu_internals.c
        ../api/src/dpu_loader.c
        ../api/src/dpu_profiler.c
        ../api/src/dpu_program.c
        ../api/src/dpu_management.c
        ../api/src/dpu_memory.c
        ../api/src/dpu_rank_handler_allocator.c
        ../api/src/dpu_runner.c

        ../api/src/runtime_info.c

        ../ufi/src/ufi.c
        ../ufi/src/ufi_bit_config.c
        ../ufi/src/ufi_dma_wavegen_config.c
        ../ufi/src/ufi_ci.c
        ../ufi/src/ufi_config.c
        ../ufi/src/ufi_debug.c
        ../ufi/src/ufi_memory.c
        ../ufi/src/ufi_runner.c
        )

set(ALL_SOURCES ${SOURCES} ${COMMONS_SOURCES})

if(ENABLE_DEBUG)
	message(STATUS "Add -g for /example")
	add_definitions("-g")
endif()

add_definitions("-D UINT32")

add_executable(checksum_host ./checksum_host.c ${ALL_SOURCES} )
target_include_directories( checksum_host PUBLIC ${INCLUDE_DIRECTORIES} )
target_link_libraries( checksum_host dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES} dpuverbose )
target_compile_definitions(checksum_host PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(checksum_host PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(checksum_host gen_files)

add_executable(test_host ./test.c ${ALL_SOURCES} )
target_include_directories( test_host PUBLIC ${INCLUDE_DIRECTORIES} )
target_link_libraries( test_host dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES} dpuverbose )
target_compile_definitions(test_host PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(test_host PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(test_host gen_files)

add_executable(DPU_CPU_Bandwidth ./DPU_CPU_Bandwidth.c ${ALL_SOURCES} )
target_include_directories( DPU_CPU_Bandwidth PUBLIC ${INCLUDE_DIRECTORIES} )
target_link_libraries( DPU_CPU_Bandwidth dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES} dpuverbose )
target_compile_definitions(DPU_CPU_Bandwidth PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(DPU_CPU_Bandwidth PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(DPU_CPU_Bandwidth gen_files)


#Device target
set(INPUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/checksum.c")
set(OUTPUT_FILE "checksum")

add_custom_command(
  OUTPUT ${OUTPUT_FILE}
  COMMAND dpu-upmem-dpurte-clang ${INPUT_FILE} -o ${OUTPUT_FILE}
  DEPENDS ${INPUT_FILE}
  COMMENT "Generating ${OUTPUT_FILE}"
)

set(INPUT_FILE2 "${CMAKE_CURRENT_SOURCE_DIR}/dpu_user.c")
set(OUTPUT_FILE2 "dpu_user")

add_custom_command(
  OUTPUT ${OUTPUT_FILE2}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=16 ${INPUT_FILE2} -o ${OUTPUT_FILE2}
  DEPENDS ${INPUT_FILE2}
  COMMENT "Generating ${OUTPUT_FILE2}"
)

set(INPUT_FILE3 "${CMAKE_CURRENT_SOURCE_DIR}/dpu_user2.c")
set(OUTPUT_FILE3 "dpu_user2")

add_custom_command(
  OUTPUT ${OUTPUT_FILE3}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=16 ${INPUT_FILE3} -o ${OUTPUT_FILE3}
  DEPENDS ${INPUT_FILE3}
  COMMENT "Generating ${OUTPUT_FILE3}"
)

add_custom_target(
  generate_output
  ALL
  DEPENDS ${OUTPUT_FILE}
  DEPENDS ${OUTPUT_FILE2}
  DEPENDS ${OUTPUT_FILE3}
)