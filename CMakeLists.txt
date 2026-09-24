cmake_minimum_required(VERSION 3.28)

find_package(Freetype)
find_package(Vulkan REQUIRED COMPONENTS glslc)
find_program(glslc_executable REQUIRED NAMES glslc HINTS Vulkan::glslc)

set(CMAKE_C_COMPILER clang)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_LINKER_TYPE LLD)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(slugdemo VERSION 0.0.1)
enable_language(ASM_NASM C)

add_subdirectory(cdk)
add_subdirectory(cvulkan)

set(vs_path_out ${CMAKE_BINARY_DIR}/vs.spv)
set(fs_path_out ${CMAKE_BINARY_DIR}/fs.spv)

add_custom_command(
  OUTPUT ${vs_path_out}
  DEPENDS ${CMAKE_SOURCE_DIR}/shader.glsl
  COMMAND
    ${glslc_executable}
    -DVERTEX_SHADER
    -fshader-stage=vertex
    --target-env=vulkan1.3
    -o vs.spv
    ${CMAKE_SOURCE_DIR}/shader.glsl
)
add_custom_command(
  OUTPUT ${fs_path_out}
  DEPENDS ${CMAKE_SOURCE_DIR}/shader.glsl
  COMMAND
    ${glslc_executable}
    -DFRAGMENT_SHADER
    -fshader-stage=fragment
    --target-env=vulkan1.3
    -o fs.spv
    ${CMAKE_SOURCE_DIR}/shader.glsl
)
add_custom_target(run_glslc DEPENDS ${vs_path_out} ${fs_path_out})
set_property(SOURCE shader.s PROPERTY LANGUAGE ASM_NASM)
add_executable(slugdemo shader.s slugdemo.c)
target_link_libraries(slugdemo cdk cvulkan Freetype::Freetype)
target_include_directories(slugdemo PRIVATE ${CMAKE_BINARY_PATH})
add_dependencies(slugdemo run_glslc)
