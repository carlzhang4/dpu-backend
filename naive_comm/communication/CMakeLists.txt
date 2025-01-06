cmake_minimum_required(VERSION 3.10)
project(MyProject)

# Set the C standard
set(CMAKE_C_STANDARD 99)

# Set the compiler path for dpu_user
set(DPU_COMPILER "dpu-upmem-dpurte-clang")

# Add executables
add_executable(test test.c ./include/CommOps.c)




# Find and include DPU package
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPU REQUIRED dpu)

target_include_directories(test PUBLIC ${DPU_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Set compilation options for naive_alltoall
target_compile_options(test PRIVATE -O3 -g ${DPU_CFLAGS})
target_link_libraries(test PRIVATE ${DPU_LIBRARIES} pthread)

set(INPUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/dpu_user.c")
set(OUTPUT_FILE "dpu_user")

add_custom_command(
  OUTPUT ${OUTPUT_FILE}
  COMMAND dpu-upmem-dpurte-clang ${INPUT_FILE} -o ${OUTPUT_FILE}
  DEPENDS ${INPUT_FILE}
  COMMENT "Generating ${OUTPUT_FILE}"
)

add_custom_target(
  generate_output_files
  ALL
  DEPENDS ${OUTPUT_FILE}
)