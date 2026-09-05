# The document generators and the headers they and their tests share. The generators are ordinary consumers of the
# installed headers: nothing here is compiled into the libraries.
add_library(gnuradio-doctools INTERFACE)
add_library(gnuradio4::gnuradio-doctools ALIAS gnuradio-doctools)
target_include_directories(gnuradio-doctools INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(gnuradio-doctools INTERFACE gnuradio-options gnuradio-core gnuradio-blocklib-core)
# the project version is not in config.hpp, so the build hands it to the tool that reports it
target_compile_definitions(gnuradio-doctools INTERFACE GR_TOOLS_CORE_VERSION="${PROJECT_VERSION}")

add_executable(gnuradio_4_0_graph_doc graph_doc_main.cpp)
target_link_libraries(gnuradio_4_0_graph_doc PRIVATE gnuradio-doctools)
install(TARGETS gnuradio_4_0_graph_doc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# the registry document is read from gr::globalBlockRegistry(), which only a build with the registry defines
if(GR_ENABLE_BLOCK_REGISTRY)
  add_executable(gnuradio_4_0_registry_doc registry_doc_main.cpp)
  target_link_libraries(gnuradio_4_0_registry_doc PRIVATE gnuradio-doctools)
  if(TARGET gnuradio-plugin)
    target_link_libraries(gnuradio_4_0_registry_doc PRIVATE gnuradio-plugin)
  endif()
  install(TARGETS gnuradio_4_0_registry_doc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(ENABLE_TESTING)
  add_subdirectory(test)
endif()
