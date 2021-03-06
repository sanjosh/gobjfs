#
# See
# https://cmake.org/cmake/help/v3.0/manual/cmake-commands.7.html
# http://www.slideshare.net/DanielPfeifer1/cmake-48475415
#
cmake_minimum_required(VERSION 2.8 FATAL_ERROR)

# Project(<name> VERSION <version> LANGUAGES CXX)
project(GOBJFS C CXX ASM)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake" "${CMAKE_MODULE_PATH}")

# Include the thread, address, memory sanitizer cmake files
include(FindUBSan)
include(FindTSan)
include(FindASan)
include(FindMSan)

# so that "make" will echo the command line for easy debugging
set(CMAKE_VERBOSE_MAKEFILE on)

# Specify command-line options
# OPTION(<option_variable> "help string describing option" [initial_value])

# CMAKE_BUILD_TYPE can be Release/Debug/RelWithDebInfo/MinSizeRel
# For Sanitizer builds, set it to ASan/MSan/TSan.  
# See the CMake directory for sanitizer cmake files
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type selected, default to Release")
  message(STATUS "To change pass -DCMAKE_BUILD_TYPE on command line")
  set(CMAKE_BUILD_TYPE "Release")
endif()

# turns on colors in generated Makefile
set(CMAKE_COLOR_MAKEFILE)

# overwriting the source and binary directories with the current ones
# this is useful for other projects reusing this project as a subcomponent
set(CMAKE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

# this will prohibit in-source-builds
if(${CMAKE_BINARY_DIR} STREQUAL ${CMAKE_SOURCE_DIR})
  message(STATUS "In-source-builds are not allowed")
  message(STATUS "Clean your source directory (e.g. delete the CMakeCache.txt file)")
  message(FATAL_ERROR "Please create a separate build directory and call CMake again")
endif(${CMAKE_BINARY_DIR} STREQUAL ${CMAKE_SOURCE_DIR})
#
# all headers specified wrt the top-level src dir
include_directories(${PROJECT_SOURCE_DIR}/src)
include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/include)

link_directories(${PROJECT_SOURCE_DIR}/lib)
link_directories(${PROJECT_SOURCE_DIR}/thirdparty/xio/lib)
link_directories(/usr/local/lib)

set(CUSTOM_LIBRARY_PATH ${PROJECT_SOURCE_DIR}/lib)

# Do not set the Compiler in this file
# Instead, set the env variables CC and CXX
# See https://cmake.org/Wiki/CMake_FAQ#How_do_I_use_a_different_compiler.3F
# SET(CMAKE_CXX_COMPILER "/usr/bin/g++")
# SET(CMAKE_C_COMPILER "/usr/bin/gcc")

set (CXX_STANDARD "c++11")

# separate out the C++ options which are not used in C 
# add boost logger macro
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=${CXX_STANDARD} -Wnon-virtual-dtor -Woverloaded-virtual -DBOOST_LOG_DYN_LINK")

# these below apply to C and C++ files
add_compile_options(
  -Wall
  -Wextra
  -Wstrict-aliasing
  -Wno-unused-parameter
  -Wno-missing-field-initializers
  -Wchar-subscripts
  -Wpointer-arith
  -Wformat
  -Wformat-security
  -Werror=format-security
  -fstack-protector-all
  -fPIE
  -fpie
  -fPIC
  -fpic
  -pipe
  -fdata-sections
  -ffunction-sections
)

# Include cppcheck target.  must come after compiler flags defined
include(cppcheck)
include(clang-format)

# to enable tests, add this to top-level CMakeLists.txt
enable_testing()
# then add this command in any subdir
# ADD_TEST(testname <executable> [arg1...])
# this will enable "make test" to run tests

# add subdirs which have CMakeLists files
# this should be done after all options are specified
# otherwise they are not visible in subdirs
# All third-pary libs should really use ExternalProject_Add
# but hack for now

add_subdirectory(src)
add_subdirectory(benchmark)
add_subdirectory(examples)

# Install the header files
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include" 
  DESTINATION "${CMAKE_INSTALL_PREFIX}")

# Set compiler options
# if (CMAKE_COMPILER_IS_GNUCXX)
#   target_compile_options(<executable>,
#	  PUBLIC -fno-elide-constructors
#    )
# endif()

# Set required compiler features
# TARGET_COMPILE_FEATURES(<executable>
#   PUBLIC
#     cxx_auto_type
#   PRIVATE
#     cxx_variadic_templates
#   )

# To generate a library called lib<libname>.so, add
# ADD_LIBRARY(<libname> STATIC/MODULE/SHARED/INTERFACE ${SOURCES})
# to set location for library installation
# INSTALL(TARGETS hello DESTINATION bin)

# to manually add sources, you can use
# set(SOURCES src/filename.cpp)

# specify sources in the executable
# ADD_EXECUTABLE(<executable_name> ${SOURCES})
# to specify library to use when linking an executable, use
# TARGET_LINK_LIBRARIES(<target_name> debug/optimized/general <dependent_library>)
# target command must be in same dir as corresponding add_library cmd

# for file IO
# FILE(DOWNLOAD...)

# String functions
# STRING(REGEX ..)

# Operations on a List
# LIST(GET/LENGTH/REVERSE/ etc)

# Execute a process commands
# EXECUTE_PROCESS(COMMAND INPUT_FILE TIMEOUT etc)

# if-then
# if (EXISTS ${ABC})
#    list(APPEND <listname> ${ABC})
# else()
#    message(FATAL_ERROR "not found")
# endif()
