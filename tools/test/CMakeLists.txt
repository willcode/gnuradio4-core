set(GR_TOOLS_TEST_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets)

add_ut_test(qa_GraphDoc)
target_link_libraries(qa_GraphDoc PRIVATE gnuradio-doctools)
target_compile_definitions(qa_GraphDoc PRIVATE GR_TOOLS_TEST_ASSETS="${GR_TOOLS_TEST_ASSETS}")

# The pinned documents are what a reader would review, so they are compared as files rather than asserted piecewise: a
# formatting change shows up as a diff of the document itself. They carry the `.expected` extension because the document
# formatters that run over this repository would rewrite a file named `.md`, and a rewritten expectation pins nothing.
function(add_pinned_document_test TEST_NAME FORMAT)
  add_test(
    NAME ${TEST_NAME}
    COMMAND
      ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:gnuradio_4_0_graph_doc> -DINPUT=${GR_TOOLS_TEST_ASSETS}/nested_graph.yaml
      -DFORMAT=${FORMAT} "-DTITLE=Nested graph fixture"
      -DACTUAL=${CMAKE_CURRENT_BINARY_DIR}/nested_graph.${FORMAT}.actual
      -DEXPECTED=${GR_TOOLS_TEST_ASSETS}/nested_graph.${FORMAT}.expected -P
      ${CMAKE_CURRENT_SOURCE_DIR}/compare_generated.cmake)
  set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 60)
endfunction()

add_pinned_document_test(qa_GraphDocPinnedMarkdown md)
add_pinned_document_test(qa_GraphDocPinnedHtml html)

# a tool asked for help says so and succeeds; a tool given an option it does not know refuses
function(add_option_handling_tests TOOL_TARGET)
  add_test(NAME qa_${TOOL_TARGET}_help COMMAND ${TOOL_TARGET} --help)
  add_test(NAME qa_${TOOL_TARGET}_unknown_option COMMAND ${TOOL_TARGET} --no-such-option)
  add_test(NAME qa_${TOOL_TARGET}_missing_value COMMAND ${TOOL_TARGET} --format)
  set_tests_properties(qa_${TOOL_TARGET}_unknown_option qa_${TOOL_TARGET}_missing_value PROPERTIES WILL_FAIL TRUE)
endfunction()

add_option_handling_tests(gnuradio_4_0_graph_doc)

if(GR_ENABLE_BLOCK_REGISTRY)
  add_ut_test(qa_RegistryDoc)
  target_link_libraries(qa_RegistryDoc PRIVATE gnuradio-doctools)
  add_option_handling_tests(gnuradio_4_0_registry_doc)
  if(INTERNAL_ENABLE_BLOCK_PLUGINS)
    # the registry document is built over core's own test plugins, which are built beside them
    target_link_libraries(qa_RegistryDoc PRIVATE gnuradio-plugin)
    target_compile_definitions(qa_RegistryDoc
                               PRIVATE GR_TOOLS_CORE_TEST_PLUGINS="${CMAKE_BINARY_DIR}/core/test/plugins")
    add_dependencies(
      qa_RegistryDoc
      GoodBasePlugin
      GoodRegistryPlugin
      good_math_plugin
      good_conversion_plugin
      bad_plugin)
  endif()
endif()
