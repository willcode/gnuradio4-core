# Configure one block library four times in one build directory. Its two headers, Gain.hpp and Gain_ext.hpp, share the
# prefix of their generated file names. Each is dropped from the list once and listed again. Once a header is removed
# from the list, the generation directory holds no file generated from it, and the merged declarations and calls name
# the units of listed headers alone. The other header's files are untouched, and so are files the generators did not
# write. A header that returns to the list gets its units back. The fixture is copied under a directory whose name holds
# a character outside ASCII, which the header paths inside the generated units then carry.
#
# The four configures group the definition units by DEFINITIONS_PER_TU, 1 by default. A fifth configure groups them by
# another count and a sixth returns to the first. Each header then holds the definition units of the current grouping
# alone.
#
# cmake -DPROJECT_DIR=<fixture project> -DMACROS=<configured macros> -DWORK_DIR=<dir> [-DGENERATOR=<cmake generator>]
# [-DCXX_COMPILER=<compiler>] [-DDEFINITIONS_PER_TU=<n>] -P RunDroppedHeaderTest.cmake

cmake_minimum_required(VERSION 3.28)

foreach(_required PROJECT_DIR MACROS WORK_DIR)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "RunDroppedHeaderTest: ${_required} is required")
  endif()
endforeach()

if(NOT DEFINED DEFINITIONS_PER_TU)
  set(DEFINITIONS_PER_TU 1)
endif()

set(_source "${WORK_DIR}/dropped_header_fixture_é")
file(REMOVE_RECURSE "${_source}")
file(COPY "${PROJECT_DIR}/" DESTINATION "${_source}")
set(_gain "${_source}/Gain.hpp")
set(_gain_ext "${_source}/Gain_ext.hpp")
set(_hand_written "hand_written.cpp" "hand_written.hpp.in")
set(_build "${WORK_DIR}/dropped_header")
set(_generated "${_build}/plugins/DroppedHeaderLib")
file(REMOVE_RECURSE "${_build}")

set(_generator_option "")
if(GENERATOR)
  list(
    APPEND
    _generator_option
    -G
    "${GENERATOR}")
endif()
if(CXX_COMPILER)
  list(APPEND _generator_option "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()

function(
  gr_configure_library
  STEP
  HEADERS
  DEFINITIONS)
  execute_process(
    COMMAND ${CMAKE_COMMAND} ${_generator_option} -S "${_source}" -B "${_build}" "-DMACROS=${MACROS}"
            "-DHEADERS=${HEADERS}" "-DGR_BLOCKLIB_DEFINITIONS_PER_TU=${DEFINITIONS}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error)
  if(NOT
     _result
     EQUAL
     0)
    message(FATAL_ERROR "${STEP}: the configure failed\n${_output}\n${_error}")
  endif()
endfunction()

# the units of a header carry its file name, then a unit number, in their own names and in the symbols of their
# initializers; the match is exact, so Gain's units never include Gain_ext's
function(
  gr_expect_units
  STEP
  HEADER
  LISTED)
  get_filename_component(_stem "${HEADER}" NAME_WE)
  file(
    GLOB _all
    RELATIVE "${_generated}"
    "${_generated}/*")
  set(_units ${_all})
  list(
    FILTER
    _units
    INCLUDE
    REGEX
    "^${_stem}_(block_)?[0-9]+(_[0-9]+)?(\\.cpp|_declarations\\.hpp\\.in|_raw_calls\\.hpp\\.in)$")
  file(READ "${_generated}/declarations.hpp" _declarations)
  file(READ "${_generated}/raw_calls.hpp" _calls)
  string(
    REGEX MATCH
          "_${_stem}_[0-9]"
          _in_declarations
          "${_declarations}")
  string(
    REGEX MATCH
          "_${_stem}_[0-9]"
          _in_calls
          "${_calls}")
  if(LISTED)
    if(NOT _units
       OR NOT _in_declarations
       OR NOT _in_calls)
      message(FATAL_ERROR "${STEP}: ${_stem} is listed, but its units are missing (files: '${_units}')")
    endif()
  else()
    if(_units)
      message(FATAL_ERROR "${STEP}: ${_stem} left the list, but its generated files remain: ${_units}")
    endif()
    if(_in_declarations OR _in_calls)
      message(FATAL_ERROR "${STEP}: ${_stem} left the list, but the merged declarations or calls still name its units")
    endif()
  endif()
endfunction()

# each fixture header registers two (block, type) entries, so a grouping of 1 gives it two definition units and any
# other grouping one; together the units define each of the two registrations once
function(
  gr_expect_definition_units
  STEP
  HEADER
  DEFINITIONS)
  get_filename_component(_stem "${HEADER}" NAME_WE)
  file(
    GLOB _definitions
    RELATIVE "${_generated}"
    "${_generated}/${_stem}_block_*.cpp")
  list(
    FILTER
    _definitions
    INCLUDE
    REGEX
    "^${_stem}_block_[0-9]+\\.cpp$")
  list(LENGTH _definitions _count)
  if(DEFINITIONS EQUAL 1)
    set(_expected_count 2)
  else()
    set(_expected_count 1)
  endif()
  if(NOT
     _count
     EQUAL
     _expected_count)
    message(FATAL_ERROR "${STEP}: ${_stem} holds the definition units '${_definitions}', "
                        "not ${_expected_count} for ${DEFINITIONS} definitions per unit")
  endif()
  set(_text "")
  foreach(_name IN LISTS _definitions)
    file(READ "${_generated}/${_name}" _unit)
    string(APPEND _text "${_unit}")
  endforeach()
  foreach(_index 0 1)
    string(
      REGEX MATCHALL
            "BlockRegistration& gr_blocklib_registration_DroppedHeaderLib_${_stem}_${_index}\\(\\) {"
            _defined
            "${_text}")
    list(LENGTH _defined _defined_count)
    if(NOT
       _defined_count
       EQUAL
       1)
      message(FATAL_ERROR "${STEP}: the definition units of ${_stem} define registration ${_index} "
                          "${_defined_count} times, not once")
    endif()
  endforeach()
endfunction()

function(gr_expect_hand_written STEP)
  foreach(_name IN LISTS _hand_written)
    if(NOT EXISTS "${_generated}/${_name}")
      message(FATAL_ERROR "${STEP}: ${_name}, which no generator wrote, was deleted")
    endif()
  endforeach()
endfunction()

gr_configure_library("both headers" "${_gain};${_gain_ext}" ${DEFINITIONS_PER_TU})
gr_expect_units("both headers" "${_gain}" TRUE)
gr_expect_units("both headers" "${_gain_ext}" TRUE)
gr_expect_definition_units("both headers" "${_gain}" ${DEFINITIONS_PER_TU})

foreach(_name IN LISTS _hand_written)
  file(WRITE "${_generated}/${_name}" "// written by hand\n")
endforeach()

gr_configure_library("Gain_ext dropped" "${_gain}" ${DEFINITIONS_PER_TU})
gr_expect_units("Gain_ext dropped" "${_gain}" TRUE)
gr_expect_units("Gain_ext dropped" "${_gain_ext}" FALSE)
gr_expect_hand_written("Gain_ext dropped")

gr_configure_library("Gain dropped, Gain_ext listed again" "${_gain_ext}" ${DEFINITIONS_PER_TU})
gr_expect_units("Gain dropped, Gain_ext listed again" "${_gain}" FALSE)
gr_expect_units("Gain dropped, Gain_ext listed again" "${_gain_ext}" TRUE)
gr_expect_hand_written("Gain dropped, Gain_ext listed again")

gr_configure_library("both listed again" "${_gain};${_gain_ext}" ${DEFINITIONS_PER_TU})
gr_expect_units("both listed again" "${_gain}" TRUE)
gr_expect_units("both listed again" "${_gain_ext}" TRUE)
gr_expect_hand_written("both listed again")

if(DEFINITIONS_PER_TU EQUAL 1)
  set(_other_definitions 3)
else()
  set(_other_definitions 1)
endif()
foreach(_definitions ${_other_definitions} ${DEFINITIONS_PER_TU})
  set(_step "${_definitions} definitions per unit")
  gr_configure_library("${_step}" "${_gain};${_gain_ext}" ${_definitions})
  gr_expect_units("${_step}" "${_gain}" TRUE)
  gr_expect_units("${_step}" "${_gain_ext}" TRUE)
  gr_expect_definition_units("${_step}" "${_gain}" ${_definitions})
  gr_expect_definition_units("${_step}" "${_gain_ext}" ${_definitions})
  gr_expect_hand_written("${_step}")
endforeach()

message(
  STATUS "the units of a dropped header are deleted, the units of a header with a longer name that starts the same "
         "stay, files no generator wrote stay, a header listed again is generated again, and a change of the grouping "
         "leaves the definition units of the current grouping alone")
