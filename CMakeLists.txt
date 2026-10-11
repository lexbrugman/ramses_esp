# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Build version, set by CI to the short commit SHA (see firmware.yml). OTA
# compares it against the version.txt published with the image, so it must
# match exactly; local builds are "dev" and always see an update.
if(DEFINED ENV{FW_VERSION})
  set(PROJECT_VER "$ENV{FW_VERSION}")
else()
  set(PROJECT_VER "dev")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ramses_esp)
