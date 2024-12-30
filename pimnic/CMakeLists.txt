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
	message(STATUS "Add -g for /libpimnic")
	add_definitions("-g")
endif()

set( PIMNIC_SOURCES
		../pimnic/entities.cpp
        )

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx512vl -mavx512bw -mclwb -mclflushopt")
add_library(pimnic SHARED ${PIMNIC_SOURCES})
target_link_libraries(pimnic dpu utils)
target_include_directories(pimnic PUBLIC ${INCLUDE_DIRECTORIES})
set_target_properties(pimnic PROPERTIES LINKER_LANGUAGE CXX)
add_dependencies(pimnic dpu)