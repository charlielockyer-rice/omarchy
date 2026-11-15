cmake_minimum_required(VERSION 3.27)

project(omarchy-hyprland-workspace-backgrounds
  DESCRIPTION "A background for every Omarchy workspace, moving with it"
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)

# Built as workspace-backgrounds.so (no lib prefix). Omarchy loads it from the
# fixed path /usr/lib/omarchy-hyprland-workspace-backgrounds/workspace-backgrounds.so.
add_library(workspace-backgrounds SHARED main.cpp)
set_target_properties(workspace-backgrounds PROPERTIES PREFIX "")
# Hyprland exposes inline globals through its headers, so whole-library hidden
# visibility breaks the host API. Our own symbols sit in an anonymous namespace.
target_compile_options(workspace-backgrounds PRIVATE -fno-gnu-unique)

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
  hyprland
  hyprgraphics
  cairo
  libdrm
  libinput
  libudev
  pixman-1
  wayland-server
  xkbcommon
)
target_link_libraries(workspace-backgrounds PRIVATE PkgConfig::deps)

install(TARGETS workspace-backgrounds LIBRARY DESTINATION lib/omarchy-hyprland-workspace-backgrounds)
