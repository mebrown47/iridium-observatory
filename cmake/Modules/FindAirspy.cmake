# - Find LIBAIRSPY
# Finds the Airspy R2 / Mini library (libairspy). Not libairspyhf, which
# drives the Airspy HF+ and is a separate library with a separate API.
#
# This module defines
#  LIBAIRSPY_INCLUDE_DIR, the directory containing libairspy/airspy.h
#  LIBAIRSPY_LIBRARIES, the libraries needed to use libairspy
#  LIBAIRSPY_FOUND, if false, do not try to use libairspy

find_package(PkgConfig)
pkg_check_modules(PC_LIBAIRSPY QUIET libairspy)

# Searched as libairspy/airspy.h rather than airspy.h with a PATH_SUFFIXES
# of libairspy, so that what lands in LIBAIRSPY_INCLUDE_DIR is the directory
# the source actually includes relative to. Finding the header's own
# directory would leave #include <libairspy/airspy.h> unresolvable.
find_path(LIBAIRSPY_INCLUDE_DIR
    NAMES libairspy/airspy.h
    HINTS
        $ENV{LIBAIRSPY_DIR}/include
        ${PC_LIBAIRSPY_INCLUDEDIR}
        ${PC_LIBAIRSPY_INCLUDE_DIRS}
        /opt/homebrew/include
        /opt/local/include
        /home/linuxbrew/.linuxbrew/include
        /usr/include
        /usr/local/include
)

find_library(LIBAIRSPY_LIBRARY
    NAMES airspy
    HINTS
        $ENV{LIBAIRSPY_DIR}/lib
        ${PC_LIBAIRSPY_LIBDIR}
        ${PC_LIBAIRSPY_LIBRARY_DIRS}
        /opt/homebrew/lib
        /opt/local/lib
        /usr/lib
        /usr/local/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Airspy DEFAULT_MSG
    LIBAIRSPY_LIBRARY LIBAIRSPY_INCLUDE_DIR)

# find_package_handle_standard_args sets AIRSPY_FOUND (after the module
# name); the rest of this project reads LIBAIRSPY_FOUND, so mirror it here
# rather than making CMakeLists.txt normalise it the way it has to for
# FindHackRF.
if(AIRSPY_FOUND)
    set(LIBAIRSPY_FOUND TRUE)
    set(LIBAIRSPY_LIBRARIES ${LIBAIRSPY_LIBRARY})
else()
    set(LIBAIRSPY_FOUND FALSE)
endif()

mark_as_advanced(LIBAIRSPY_INCLUDE_DIR LIBAIRSPY_LIBRARY)
