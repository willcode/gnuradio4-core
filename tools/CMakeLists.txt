# The document generators and the headers they and their tests share. The generators are ordinary consumers of the
# installed headers: nothing here is compiled into the libraries.
add_library(gnuradio-doctools INTERFACE)
add_library(gnuradio4::gnuradio-doctools ALIAS gnuradio-doctools)
target_include_directories(gnuradio-doctools INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(gnuradio-doctools INTERFACE gnuradio-options gnuradio-core gnuradio-blocklib-core)

add_executable(gnuradio_4_0_graph_doc graph_doc_main.cpp)
target_link_libraries(gnuradio_4_0_graph_doc PRIVATE gnuradio-doctools)
install(TARGETS gnuradio_4_0_graph_doc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(ENABLE_TESTING)
  add_subdirectory(test)
endif()
