# - Find SDL3_mixer library and headers
#
# Find module for SDL_mixer 3.0 (https://www.libsdl.org/projects/SDL_mixer/).
# It defines the following variables:
#  SDL3_MIXER_INCLUDE_DIRS - The location of the headers, e.g., SDL3/SDL_mixer.h.
#  SDL3_MIXER_LIBRARIES - The libraries to link against to use SDL3_mixer.
#  SDL3_MIXER_FOUND - If false, do not try to use SDL3_mixer.
#  SDL3_MIXER_VERSION_STRING
#    Human-readable string containing the version of SDL3_mixer.
#
# Also defined, but not for general use are:
#   SDL3_MIXER_INCLUDE_DIR - The directory that contains SDL3/SDL_mixer.h.
#   SDL3_MIXER_LIBRARY - The location of the SDL3_mixer library.
#

find_package(PkgConfig QUIET)
pkg_check_modules(PC_SDL3_MIXER QUIET SDL3_mixer)

find_path(SDL3_MIXER_INCLUDE_DIR
  NAMES SDL3/SDL_mixer.h
  HINTS
    ${SDL3_MIXER_ROOT}/include
)

find_library(SDL3_MIXER_LIBRARY
  NAMES SDL3_mixer
  HINTS
    ${SDL3_MIXER_ROOT}/lib
  PATH_SUFFIXES x64
)

if(SDL3_MIXER_INCLUDE_DIR AND EXISTS "${SDL3_MIXER_INCLUDE_DIR}/SDL3/SDL_mixer.h")
  file(STRINGS "${SDL3_MIXER_INCLUDE_DIR}/SDL3/SDL_mixer.h" SDL3_MIXER_VERSION_MAJOR_LINE REGEX "^#define[ \t]+SDL_MIXER_MAJOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL3_MIXER_INCLUDE_DIR}/SDL3/SDL_mixer.h" SDL3_MIXER_VERSION_MINOR_LINE REGEX "^#define[ \t]+SDL_MIXER_MINOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL3_MIXER_INCLUDE_DIR}/SDL3/SDL_mixer.h" SDL3_MIXER_VERSION_PATCH_LINE REGEX "^#define[ \t]+SDL_MIXER_MICRO_VERSION[ \t]+[0-9]+$")
  string(REGEX REPLACE "^#define[ \t]+SDL_MIXER_MAJOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_MIXER_VERSION_MAJOR "${SDL3_MIXER_VERSION_MAJOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_MIXER_MINOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_MIXER_VERSION_MINOR "${SDL3_MIXER_VERSION_MINOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_MIXER_MICRO_VERSION[ \t]+([0-9]+)$" "\\1" SDL3_MIXER_VERSION_PATCH "${SDL3_MIXER_VERSION_PATCH_LINE}")
  set(SDL3_MIXER_VERSION_STRING ${SDL3_MIXER_VERSION_MAJOR}.${SDL3_MIXER_VERSION_MINOR}.${SDL3_MIXER_VERSION_PATCH})
  unset(SDL3_MIXER_VERSION_MAJOR_LINE)
  unset(SDL3_MIXER_VERSION_MINOR_LINE)
  unset(SDL3_MIXER_VERSION_PATCH_LINE)
  unset(SDL3_MIXER_VERSION_MAJOR)
  unset(SDL3_MIXER_VERSION_MINOR)
  unset(SDL3_MIXER_VERSION_PATCH)
endif()

set(SDL3_MIXER_INCLUDE_DIRS ${SDL3_MIXER_INCLUDE_DIR})
set(SDL3_MIXER_LIBRARIES ${SDL3_MIXER_LIBRARY})

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(SDL3_mixer
                                  REQUIRED_VARS SDL3_MIXER_INCLUDE_DIR SDL3_MIXER_LIBRARY
                                  VERSION_VAR SDL3_MIXER_VERSION_STRING)

mark_as_advanced(SDL3_MIXER_INCLUDE_DIR SDL3_MIXER_LIBRARY)
