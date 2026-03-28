# - Find SDL3_image library and headers
#
# Find module for SDL_image 3.0 (https://www.libsdl.org/projects/SDL_image/).
# It defines the following variables:
#  SDL3_IMAGE_INCLUDE_DIRS - The location of the headers, e.g., SDL3/SDL_image.h.
#  SDL3_IMAGE_LIBRARIES - The libraries to link against to use SDL3_image.
#  SDL3_IMAGE_FOUND - If false, do not try to use SDL3_image.
#  SDL3_IMAGE_VERSION_STRING
#    Human-readable string containing the version of SDL3_image.
#
# Also defined, but not for general use are:
#   SDL3_IMAGE_INCLUDE_DIR - The directory that contains SDL3/SDL_image.h.
#   SDL3_IMAGE_LIBRARY - The location of the SDL3_image library.
#

find_package(PkgConfig QUIET)
pkg_check_modules(PC_SDL3_IMAGE QUIET SDL3_image)

find_path(SDL3_IMAGE_INCLUDE_DIR
  NAMES SDL3/SDL_image.h
  HINTS
    ${SDL3_IMAGE_ROOT}/include
)

find_library(SDL3_IMAGE_LIBRARY
  NAMES SDL3_image
  HINTS
    ${SDL3_IMAGE_ROOT}/lib
  PATH_SUFFIXES x64
)

if(SDL3_IMAGE_INCLUDE_DIR AND EXISTS "${SDL3_IMAGE_INCLUDE_DIR}/SDL3/SDL_image.h")
  file(STRINGS "${SDL3_IMAGE_INCLUDE_DIR}/SDL3/SDL_image.h" SDL3_IMAGE_VERSION_MAJOR_LINE REGEX "^#define[ \t]+SDL_IMAGE_MAJOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL3_IMAGE_INCLUDE_DIR}/SDL3/SDL_image.h" SDL3_IMAGE_VERSION_MINOR_LINE REGEX "^#define[ \t]+SDL_IMAGE_MINOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL3_IMAGE_INCLUDE_DIR}/SDL3/SDL_image.h" SDL3_IMAGE_VERSION_PATCH_LINE REGEX "^#define[ \t]+SDL_IMAGE_MICRO_VERSION[ \t]+[0-9]+$")
  string(REGEX REPLACE "^#define[ \t]+SDL_IMAGE_MAJOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_IMAGE_VERSION_MAJOR "${SDL3_IMAGE_VERSION_MAJOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_IMAGE_MINOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_IMAGE_VERSION_MINOR "${SDL3_IMAGE_VERSION_MINOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_IMAGE_MICRO_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_IMAGE_VERSION_PATCH "${SDL3_IMAGE_VERSION_PATCH_LINE}")
  set(SDL3_IMAGE_VERSION_STRING ${SDL3_IMAGE_VERSION_MAJOR}.${SDL3_IMAGE_VERSION_MINOR}.${SDL3_IMAGE_VERSION_PATCH})
  unset(SDL3_IMAGE_VERSION_MAJOR_LINE)
  unset(SDL3_IMAGE_VERSION_MINOR_LINE)
  unset(SDL3_IMAGE_VERSION_PATCH_LINE)
  unset(SDL3_IMAGE_VERSION_MAJOR)
  unset(SDL3_IMAGE_VERSION_MINOR)
  unset(SDL3_IMAGE_VERSION_PATCH)
endif()

set(SDL3_IMAGE_INCLUDE_DIRS ${SDL3_IMAGE_INCLUDE_DIR})
set(SDL3_IMAGE_LIBRARIES ${SDL3_IMAGE_LIBRARY})

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(SDL3_image
                                  REQUIRED_VARS SDL3_IMAGE_INCLUDE_DIR SDL3_IMAGE_LIBRARY
                                  VERSION_VAR SDL3_IMAGE_VERSION_STRING)

mark_as_advanced(SDL3_IMAGE_INCLUDE_DIR SDL3_IMAGE_LIBRARY)
