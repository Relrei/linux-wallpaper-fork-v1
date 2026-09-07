cmake_minimum_required(VERSION 3.16)
project(linux-wallpaper-fork LANGUAGES C CXX)

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

add_executable(linux-wallpaper-fork
  src/main.cpp
  src/xdg-shell-protocol.c
  src/wlr-layer-shell-unstable-v1-protocol.c
)
target_include_directories(linux-wallpaper-fork PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${WAYLAND_INCLUDE_DIRS}
)
target_compile_options(linux-wallpaper-fork PRIVATE -Wall -Wextra -Wno-unused-parameter ${WAYLAND_CFLAGS_OTHER})
target_link_libraries(linux-wallpaper-fork PRIVATE
  spine-gl
  spine-cpp
  spine-cpp-lite
  ${WAYLAND_LIBRARIES}
  GLEW::GLEW
  OpenGL::GL
  m
)

# Keep the previous build path as a compatibility copy for existing scripts.
add_custom_command(TARGET linux-wallpaper-fork POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:linux-wallpaper-fork>
          ${CMAKE_BINARY_DIR}/linux-wallpaper-fork
)

install(TARGETS linux-wallpaper-fork RUNTIME DESTINATION bin)
