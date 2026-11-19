cmake_minimum_required(VERSION 3.16)
project(brigandine_pc C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL2 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GEN_DIR ${CMAKE_SOURCE_DIR}/build/gen)

# Regenerate recompiled code when the recompiler or HLE list changes.
add_custom_command(
  OUTPUT ${GEN_DIR}/recomp_tables.c
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/recomp.py ${CMAKE_SOURCE_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/recomp.py ${CMAKE_SOURCE_DIR}/src/pc/hle_list.txt
  COMMENT "Recompiling PS1 code")

file(GLOB GEN_SRC ${GEN_DIR}/recomp_*.c)
list(APPEND GEN_SRC ${GEN_DIR}/recomp_tables.c)
list(REMOVE_DUPLICATES GEN_SRC)
file(GLOB PC_SRC ${CMAKE_SOURCE_DIR}/src/pc/*.c)

add_executable(brigandine ${PC_SRC} ${GEN_SRC})
target_include_directories(brigandine PRIVATE src/pc ${GEN_DIR})
target_link_libraries(brigandine PRIVATE SDL2::SDL2)
set_source_files_properties(${GEN_SRC} PROPERTIES COMPILE_OPTIONS "-w;-O1")
if(NOT MSVC)
  target_compile_options(brigandine PRIVATE -Wall -Wno-unused-function)
endif()
