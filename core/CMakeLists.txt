add_library(
  gnuradio-core STATIC
  src/Value.cpp
  src/ValueHelper.cpp
  src/Block.cpp
  src/BlockModel.cpp
  src/Port.cpp
  src/Settings.cpp
  src/Graph.cpp
  src/PluginLoader.cpp
  src/PmtTypeHelpers.cpp)

# building a graph by name needs a registry to look the names up in
if(GR_ENABLE_BLOCK_REGISTRY)
  target_sources(gnuradio-core PRIVATE src/Runtime.cpp src/SchedulerRegistration.cpp)
endif()

add_library(gnuradio4::gnuradio-core ALIAS gnuradio-core)
target_include_directories(
  gnuradio-core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/src>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
         $<INSTALL_INTERFACE:include/>)

target_link_libraries(
  gnuradio-core
  PUBLIC gnuradio-options
         gnuradio-meta
         magic_enum
         vir)

if(EMSCRIPTEN)
  add_library(gnuradio-blocklib-core STATIC src/BlockRegistry.cpp)
else()
  add_library(gnuradio-blocklib-core SHARED src/BlockRegistry.cpp)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/gnuradio-4.0/Plugin.hpp" _gr_plugin_abi_define
       REGEX "^#define[ \t]+GR_PLUGIN_CURRENT_ABI_VERSION[ \t]+[0-9]+")
  # CMake reads the define when it configures. A change to Plugin.hpp re-runs configure, and the SOVERSION follows the
  # define.
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS include/gnuradio-4.0/Plugin.hpp)
  list(LENGTH _gr_plugin_abi_define _gr_plugin_abi_define_count)
  if(NOT
     _gr_plugin_abi_define_count
     EQUAL
     1)
    message(FATAL_ERROR "Plugin.hpp defines GR_PLUGIN_CURRENT_ABI_VERSION ${_gr_plugin_abi_define_count} times, "
                        "and the block-library core needs exactly one definition for its SOVERSION")
  endif()
  string(
    REGEX
    REPLACE "^#define[ \t]+GR_PLUGIN_CURRENT_ABI_VERSION[ \t]+([0-9]+).*$"
            "\\1"
            _gr_plugin_abi_version
            "${_gr_plugin_abi_define}")
  # The SOVERSION guards the registration layout a block library was built against: the dynamic linker refuses a block
  # library that needs another number. The number is GR_PLUGIN_CURRENT_ABI_VERSION and moves with it.
  set_target_properties(gnuradio-blocklib-core PROPERTIES SOVERSION ${_gr_plugin_abi_version})
endif()
add_library(gnuradio4::gnuradio-blocklib-core ALIAS gnuradio-blocklib-core)
message(INFO "PATH include " $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/>)
target_include_directories(gnuradio-blocklib-core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/>
                                                         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(gnuradio-blocklib-core PUBLIC gnuradio-core)
if(WIN32)
  target_compile_definitions(gnuradio-core PUBLIC BUILDING_MYPLUGIN)
endif()

# configure a header file to pass the CMake settings to the source code
configure_file("${PROJECT_SOURCE_DIR}/cmake/config.hpp.in"
               "${CMAKE_CURRENT_BINARY_DIR}/include/gnuradio-4.0/config.hpp" @ONLY)
# TODO: install configure file... but not really meaningful for header only library, since compile flags are defined by
# the user...
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/gnuradio-4.0/config.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gnuradio-4.0")

install(
  TARGETS gnuradio-core gnuradio-blocklib-core
  EXPORT gnuradio4Targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# the whole include directory is the public surface, and it is installed as it is laid out: a PUBLIC_HEADER list would
# flatten the subdirectories into a second copy beside the first
if(GR_ENABLE_BLOCK_REGISTRY)
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
else()
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PATTERN "Runtime.hpp" EXCLUDE
    PATTERN "SchedulerRegistration.hpp" EXCLUDE)
endif()

add_subdirectory(src)

if(ENABLE_TESTING)
  add_subdirectory(test)
  add_subdirectory(benchmarks)
endif()
