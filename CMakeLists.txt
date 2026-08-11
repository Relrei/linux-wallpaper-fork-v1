cmake_minimum_required(VERSION 3.16)
project(nle-wallpaper-host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-egl wayland-cursor egl)

set(SPINE_RT ${CMAKE_SOURCE_DIR}/third_party/spine-runtimes)
# Keep RTTI on: GlTextureLoader inherits spine::TextureLoader and needs typeinfo.
set(SPINE_SET_COMPILER_FLAGS OFF CACHE BOOL "" FORCE)
add_subdirectory(${SPINE_RT}/spine-cpp ${CMAKE_BINARY_DIR}/spine-cpp-build)

add_library(spine-gl STATIC
  third_party/spine-gl/spine-glfw.cpp
  third_party/spine-gl/spine-glfw.h
  third_party/spine-gl/stb_image.h
)
target_include_directories(spine-gl PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/spine-gl
  ${SPINE_RT}/spine-cpp/spine-cpp/include
  ${SPINE_RT}/spine-cpp/spine-cpp-lite
)
target_compile_options(spine-gl PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(spine-gl PUBLIC spine-cpp spine-cpp-lite GLEW::GLEW OpenGL::GL)

add_executable(nle-wallpaper-host
  src/main.cpp
  src/xdg-shell-protocol.c
  src/wlr-layer-shell-unstable-v1-protocol.c
)
target_include_directories(nle-wallpaper-host PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${WAYLAND_INCLUDE_DIRS}
)
target_compile_options(nle-wallpaper-host PRIVATE -Wall -Wextra -Wno-unused-parameter ${WAYLAND_CFLAGS_OTHER})
target_link_libraries(nle-wallpaper-host PRIVATE
  spine-gl
  spine-cpp
  spine-cpp-lite
  ${WAYLAND_LIBRARIES}
  GLEW::GLEW
  OpenGL::GL
  m
)

# Keep the previous build path as a compatibility copy for existing scripts.
add_custom_command(TARGET nle-wallpaper-host POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nle-wallpaper-host>
          ${CMAKE_BINARY_DIR}/kei-wallpaper-host
)

install(TARGETS nle-wallpaper-host RUNTIME DESTINATION bin)
