cmake_minimum_required(VERSION 3.13)

cmake_policy(SET CMP0048 NEW)

set( INCLUDE_DIRECTORIES
        ../api/include/lowlevel
        ../api/include/api
        ../api/src/include
        ../commons/src/properties
        ../commons/src/pcb
        ../commons/src
        ../commons/include
        ../verbose/src
        src/commons
        src
		../bfdma
		../util
        )

if(ENABLE_DEBUG)
	message(STATUS "Add -g for /libutils")
	add_definitions("-g")
endif()

set( UTILS_SOURCES
		../util/utils.cpp
		../util/virt2phys.cpp
        )

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx512vl -mavx512bw -mclwb -mclflushopt")
add_library(utils SHARED ${UTILS_SOURCES})
# target_link_libraries(utils dpu-ufi)
target_include_directories(utils PUBLIC ${INCLUDE_DIRECTORIES})
set_target_properties(utils PROPERTIES LINKER_LANGUAGE CXX)
