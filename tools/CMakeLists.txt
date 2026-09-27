# An installed tool reaches the libraries through a run path relative to its own directory. It runs from any prefix
# without a library path in the environment. The run path of a tool in the build tree stays the one CMake sets.
file(
  RELATIVE_PATH
  _gr_tools_bin_to_lib
  "${CMAKE_INSTALL_FULL_BINDIR}"
  "${CMAKE_INSTALL_FULL_LIBDIR}")
if(APPLE)
  set(GR_TOOLS_INSTALL_RPATH "@loader_path/${_gr_tools_bin_to_lib}")
else()
  set(GR_TOOLS_INSTALL_RPATH "$ORIGIN/${_gr_tools_bin_to_lib}")
endif()

# graphdoc and the headers it shares with its test. It is an ordinary consumer of the installed headers: nothing here is
# compiled into the libraries. Its document is read from the graph file, so it is built whether or not the registry is
# on.
add_library(gnuradio-doctools INTERFACE)
add_library(gnuradio4::gnuradio-doctools ALIAS gnuradio-doctools)
target_include_directories(gnuradio-doctools INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(gnuradio-doctools INTERFACE gnuradio-options gnuradio-core gnuradio-blocklib-core)
# the project version is not in config.hpp, so the build hands it to the tool that reports it
target_compile_definitions(gnuradio-doctools INTERFACE GR_TOOLS_CORE_VERSION="${PROJECT_VERSION}")

add_executable(graphdoc graphdoc_main.cpp)
target_link_libraries(graphdoc PRIVATE gnuradio-doctools)
if(TARGET gnuradio-plugin)
  target_link_libraries(graphdoc PRIVATE gnuradio-plugin)
endif()
set_target_properties(graphdoc PROPERTIES INSTALL_RPATH "${GR_TOOLS_INSTALL_RPATH}")
install(TARGETS graphdoc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# the registry document is read from gr::globalBlockRegistry(), which only a build with the registry defines
if(GR_ENABLE_BLOCK_REGISTRY)
  add_executable(regdoc regdoc_main.cpp)
  target_link_libraries(regdoc PRIVATE gnuradio-doctools)
  if(TARGET gnuradio-plugin)
    target_link_libraries(regdoc PRIVATE gnuradio-plugin)
  endif()
  set_target_properties(regdoc PROPERTIES INSTALL_RPATH "${GR_TOOLS_INSTALL_RPATH}")
  install(TARGETS regdoc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# rungraph, an ordinary consumer of the installed headers: it is not compiled into the libraries. It builds its graph by
# name, which needs the registry, so it follows it.
if(GR_ENABLE_BLOCK_REGISTRY)
  add_executable(rungraph rungraph_main.cpp)
  target_link_libraries(rungraph PRIVATE gnuradio-options gnuradio-core gnuradio-blocklib-core)
  if(TARGET gnuradio-plugin)
    target_link_libraries(rungraph PRIVATE gnuradio-plugin)
  endif()
  set_target_properties(rungraph PROPERTIES INSTALL_RPATH "${GR_TOOLS_INSTALL_RPATH}")
  install(TARGETS rungraph RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# grinfo, an ordinary consumer of the installed headers as rungraph is. It reports the registered blocks, which only a
# build with the registry has.
if(GR_ENABLE_BLOCK_REGISTRY)
  add_executable(grinfo grinfo_main.cpp)
  target_link_libraries(grinfo PRIVATE gnuradio-options gnuradio-core gnuradio-blocklib-core)
  if(TARGET gnuradio-plugin)
    target_link_libraries(grinfo PRIVATE gnuradio-plugin)
  endif()
  # what this installation is; the project version is not in config.hpp, so the build hands it over too
  target_compile_definitions(grinfo PRIVATE GR_TOOLS_CORE_VERSION="${PROJECT_VERSION}"
                                            GR_TOOLS_INSTALLED_PREFIX="${CMAKE_INSTALL_PREFIX}")
  set_target_properties(grinfo PROPERTIES INSTALL_RPATH "${GR_TOOLS_INSTALL_RPATH}")
  install(TARGETS grinfo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(ENABLE_TESTING)
  add_subdirectory(test)
endif()
