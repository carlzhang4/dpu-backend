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

add_executable(checksum_host ./checksum_host.c ${ALL_SOURCES} )
target_include_directories( checksum_host PUBLIC ${INCLUDE_DIRECTORIES} )
target_link_libraries( checksum_host dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES} dpuverbose )
target_compile_definitions(checksum_host PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(checksum_host PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(checksum_host gen_files)


#Device target
set(INPUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/checksum.c")
set(OUTPUT_FILE "checksum")

add_custom_command(
  OUTPUT ${OUTPUT_FILE}
  COMMAND dpu-upmem-dpurte-clang ${INPUT_FILE} -o ${OUTPUT_FILE}
  DEPENDS ${INPUT_FILE}
  COMMENT "Generating ${OUTPUT_FILE}"
)

add_custom_target(
  generate_output
  ALL
  DEPENDS ${OUTPUT_FILE}
)