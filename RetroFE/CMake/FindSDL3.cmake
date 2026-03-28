# - Find SDL3 library and headers
#
# Find module for SDL 3.0 (https://www.libsdl.org/).
# It defines the following variables:
#  SDL3_INCLUDE_DIRS - The location of the headers, e.g., SDL3/SDL.h.
#  SDL3_LIBRARIES - The libraries to link against to use SDL3.
#  SDL3_FOUND - If false, do not try to use SDL3.
#  SDL3_VERSION_STRING - Human-readable string containing the version of SDL3.
#
# This module responds to the flag:
#  SDL3_BUILDING_LIBRARY
#    If this is defined, then no SDL3main will be linked in because
#    only applications need main().
#    Otherwise, it is assumed you are building an application and this
#    module will attempt to locate and set the the proper link flags
#    as part of the returned SDL3_LIBRARIES variable.
#
# Also defined, but not for general use are:
#   SDL3_INCLUDE_DIR - The directory that contains SDL3/SDL.h.
#   SDL3_LIBRARY - The location of the SDL3 library.
#   SDL3MAIN_LIBRARY - The location of the SDL3main library.
#

find_package(PkgConfig QUIET)
pkg_check_modules(PC_SDL3 QUIET sdl3)

find_path(SDL3_INCLUDE_DIR
  NAMES SDL3/SDL.h
  HINTS
    ${SDL3_ROOT}/include /opt/homebrew/include
)

find_library(SDL3_LIBRARY
  NAMES SDL3
  HINTS
    ${SDL3_ROOT}/lib
  PATH_SUFFIXES x64
)

if(NOT SDL3_BUILDING_LIBRARY)
  # Clear stale NOTFOUND from previous configure runs so find_library re-searches
  # when the package has since been downloaded into the hint directory.
  if(SDL3_ROOT AND IS_DIRECTORY "${SDL3_ROOT}/lib" AND NOT SDL3MAIN_LIBRARY)
    unset(SDL3MAIN_LIBRARY CACHE)
  endif()
  find_library(SDL3MAIN_LIBRARY
    NAMES SDL3main
    HINTS
      ${SDL3_ROOT}/lib
    PATH_SUFFIXES x64
  )
endif()

if(SDL3_INCLUDE_DIR AND EXISTS "${SDL3_INCLUDE_DIR}/SDL3/SDL_version.h")
  file(STRINGS "${SDL3_INCLUDE_DIR}/SDL3/SDL_version.h" SDL3_VERSION_MAJOR_LINE REGEX "^#define[ \t]+SDL_MAJOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL3_INCLUDE_DIR}/SDL3/SDL_version.h" SDL3_VERSION_MINOR_LINE REGEX "^#define[ \t]+SDL_MINOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL3_INCLUDE_DIR}/SDL3/SDL_version.h" SDL3_VERSION_PATCH_LINE REGEX "^#define[ \t]+SDL_MICRO_VERSION[ \t]+[0-9]+$")
  string(REGEX REPLACE "^#define[ \t]+SDL_MAJOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_VERSION_MAJOR "${SDL3_VERSION_MAJOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_MINOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_VERSION_MINOR "${SDL3_VERSION_MINOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_MICRO_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_VERSION_PATCH "${SDL3_VERSION_PATCH_LINE}")
  set(SDL3_VERSION_STRING ${SDL3_VERSION_MAJOR}.${SDL3_VERSION_MINOR}.${SDL3_VERSION_PATCH})
  unset(SDL3_VERSION_MAJOR_LINE)
  unset(SDL3_VERSION_MINOR_LINE)
  unset(SDL3_VERSION_PATCH_LINE)
  unset(SDL3_VERSION_MAJOR)
  unset(SDL3_VERSION_MINOR)
  unset(SDL3_VERSION_PATCH)
endif()

set(SDL3_INCLUDE_DIRS ${SDL3_INCLUDE_DIR})
if(SDL3MAIN_LIBRARY)
  set(SDL3_LIBRARIES ${SDL3MAIN_LIBRARY} ${SDL3_LIBRARY})
else()
  set(SDL3_LIBRARIES ${SDL3_LIBRARY})
endif()

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(SDL3
                                  REQUIRED_VARS SDL3_INCLUDE_DIR SDL3_LIBRARY
                                  VERSION_VAR SDL3_VERSION_STRING)

mark_as_advanced(SDL3_INCLUDE_DIR SDL3_LIBRARY)
