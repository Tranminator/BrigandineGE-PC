cmake_minimum_required(VERSION 3.16)
project(brigandine_pc C CXX)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL2 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Dear ImGui (MIT) for the settings window — fetched at configure time
include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
set(IMGUI_SRC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)

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
file(GLOB PC_SRC ${CMAKE_SOURCE_DIR}/src/pc/*.c ${CMAKE_SOURCE_DIR}/src/pc/*.cpp)

add_executable(brigandine ${PC_SRC} ${GEN_SRC} ${IMGUI_SRC})
target_include_directories(brigandine PRIVATE src/pc ${GEN_DIR} ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(brigandine PRIVATE SDL2::SDL2)
set_source_files_properties(${GEN_SRC} PROPERTIES COMPILE_OPTIONS "-w;-O1")
if(NOT MSVC)
  target_compile_options(brigandine PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wno-unused-function>)
endif()

# Runtime assets (bundled OFL font for the BIOS-font replacement) next to the executable
add_custom_command(TARGET brigandine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:brigandine>/assets)
