cmake_minimum_required(VERSION 3.13)

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

set(ALL_SOURCES ${SOURCES} ${COMMONS_SOURCES})

if(ENABLE_DEBUG)
	message(STATUS "Add -g for /example")
	add_definitions("-g")
endif()

add_definitions("-D UINT32")




add_executable(DPU_CPU_Bandwidth ./DPU_CPU_Bandwidth.c ${ALL_SOURCES} )
target_include_directories( DPU_CPU_Bandwidth PUBLIC ${INCLUDE_DIRECTORIES} )
target_link_libraries( DPU_CPU_Bandwidth dpuhw m ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBELF_LIBRARIES} ${Python3_LIBRARIES} ${LIBNUMA_LIBRARIES} dpuverbose )
target_compile_definitions(DPU_CPU_Bandwidth PUBLIC DPU_TOOLS_VERSION=${UPMEM_VERSION})
set_target_properties(DPU_CPU_Bandwidth PROPERTIES VERSION ${UPMEM_VERSION})
add_dependencies(DPU_CPU_Bandwidth gen_files)


#Device target
set(INPUT_DPU_CPU_Bandwidth_host "${CMAKE_CURRENT_SOURCE_DIR}/DPU_CPU_Bandwidth_host.c")
set(OUTPUT_DPU_CPU_Bandwidth_host "DPU_CPU_Bandwidth_host")

add_custom_command(
  OUTPUT ${OUTPUT_DPU_CPU_Bandwidth_host}
  COMMAND dpu-upmem-dpurte-clang -Wall -Wextra -g -Isupport -O2 -DNR_TASKLETS=16 -DBL=10 ${INPUT_DPU_CPU_Bandwidth_host} -o ${OUTPUT_DPU_CPU_Bandwidth_host}
  DEPENDS ${INPUT_DPU_CPU_Bandwidth_host}
  COMMENT "Generating ${OUTPUT_DPU_CPU_Bandwidth_host}"
)



add_custom_target(
  generate_output_files_Microbenchmark
  ALL
  DEPENDS ${OUTPUT_DPU_CPU_Bandwidth_host}
)