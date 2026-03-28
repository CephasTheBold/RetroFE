# - Find SDL3_ttf library and headers
#
# Find module for SDL_ttf 3.0 (https://www.libsdl.org/projects/SDL_ttf/).
# It defines the following variables:
#  SDL3_TTF_INCLUDE_DIRS - The location of the headers, e.g., SDL3/SDL_ttf.h.
#  SDL3_TTF_LIBRARIES - The libraries to link against to use SDL3_ttf.
#  SDL3_TTF_FOUND - If false, do not try to use SDL3_ttf.
#  SDL3_TTF_VERSION_STRING
#    Human-readable string containing the version of SDL3_ttf.
#
# Also defined, but not for general use are:
#   SDL3_TTF_INCLUDE_DIR - The directory that contains SDL3/SDL_ttf.h.
#   SDL3_TTF_LIBRARY - The location of the SDL3_ttf library.
#

find_package(PkgConfig QUIET)
pkg_check_modules(PC_SDL3_TTF QUIET SDL3_ttf)

find_path(SDL3_TTF_INCLUDE_DIR
  NAMES SDL3/SDL_ttf.h
  HINTS
    ${SDL3_TTF_ROOT}/include
)

find_library(SDL3_TTF_LIBRARY
  NAMES SDL3_ttf
  HINTS
    ${SDL3_TTF_ROOT}/lib
  PATH_SUFFIXES x64
)

if(SDL3_TTF_INCLUDE_DIR AND EXISTS "${SDL3_TTF_INCLUDE_DIR}/SDL3/SDL_ttf.h")
  file(STRINGS "${SDL3_TTF_INCLUDE_DIR}/SDL3/SDL_ttf.h" SDL3_TTF_VERSION_MAJOR_LINE REGEX "^#define[ \t]+SDL_TTF_MAJOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL3_TTF_INCLUDE_DIR}/SDL3/SDL_ttf.h" SDL3_TTF_VERSION_MINOR_LINE REGEX "^#define[ \t]+SDL_TTF_MINOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL3_TTF_INCLUDE_DIR}/SDL3/SDL_ttf.h" SDL3_TTF_VERSION_PATCH_LINE REGEX "^#define[ \t]+SDL_TTF_MICRO_VERSION[ \t]+[0-9]+$")
  string(REGEX REPLACE "^#define[ \t]+SDL_TTF_MAJOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_TTF_VERSION_MAJOR "${SDL3_TTF_VERSION_MAJOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_TTF_MINOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_TTF_VERSION_MINOR "${SDL3_TTF_VERSION_MINOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_TTF_MICRO_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_TTF_VERSION_PATCH "${SDL3_TTF_VERSION_PATCH_LINE}")
  set(SDL3_TTF_VERSION_STRING ${SDL3_TTF_VERSION_MAJOR}.${SDL3_TTF_VERSION_MINOR}.${SDL3_TTF_VERSION_PATCH})
  unset(SDL3_TTF_VERSION_MAJOR_LINE)
  unset(SDL3_TTF_VERSION_MINOR_LINE)
  unset(SDL3_TTF_VERSION_PATCH_LINE)
  unset(SDL3_TTF_VERSION_MAJOR)
  unset(SDL3_TTF_VERSION_MINOR)
  unset(SDL3_TTF_VERSION_PATCH)
endif()

set(SDL3_TTF_INCLUDE_DIRS ${SDL3_TTF_INCLUDE_DIR})
set(SDL3_TTF_LIBRARIES ${SDL3_TTF_LIBRARY})

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(SDL3_ttf
                                  REQUIRED_VARS SDL3_TTF_INCLUDE_DIR SDL3_TTF_LIBRARY
                                  VERSION_VAR SDL3_TTF_VERSION_STRING)

mark_as_advanced(SDL3_TTF_INCLUDE_DIR SDL3_TTF_LIBRARY)
