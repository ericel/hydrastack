cmake_minimum_required(VERSION 3.20)
project(HydraStack VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HYDRA_BUILD_UI "Build React/Tailwind assets via npm" OFF)
option(HYDRA_BUILD_DEMO "Build the hydra_demo application" ON)

find_package(drogon CONFIG REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)

set(V8_COMPILE_DEFINITIONS
    ""
    CACHE STRING
    "Semicolon-separated V8 compile definitions (example: V8_COMPRESS_POINTERS;V8_ENABLE_SANDBOX)")

if(TARGET drogon::drogon)
  set(HYDRA_DROGON_TARGET drogon::drogon)
elseif(TARGET Drogon::Drogon)
  set(HYDRA_DROGON_TARGET Drogon::Drogon)
else()
  message(FATAL_ERROR "Unable to find a supported Drogon target (expected drogon::drogon or Drogon::Drogon)")
endif()

if(TARGET jsoncpp_lib)
  set(HYDRA_JSONCPP_TARGET jsoncpp_lib)
elseif(TARGET jsoncpp::jsoncpp)
  set(HYDRA_JSONCPP_TARGET jsoncpp::jsoncpp)
elseif(TARGET JsonCpp::JsonCpp)
  set(HYDRA_JSONCPP_TARGET JsonCpp::JsonCpp)
else()
  message(FATAL_ERROR "Unable to find a supported jsoncpp target")
endif()

# v0.1 V8 discovery contract.
# Explicit -DV8_INCLUDE_DIR/-DV8_LIBRARIES wins.
# If missing, we try env vars and common install locations.
option(HYDRA_V8_AUTODETECT "Attempt to auto-detect local V8 install when V8_* is unset" ON)

set(HYDRA_V8_HINT_ROOTS "")
if(DEFINED ENV{V8_ROOT} AND NOT "$ENV{V8_ROOT}" STREQUAL "")
  list(APPEND HYDRA_V8_HINT_ROOTS "$ENV{V8_ROOT}")
endif()
if(APPLE)
  list(APPEND HYDRA_V8_HINT_ROOTS "/opt/homebrew/opt/v8" "/usr/local/opt/v8")
endif()
list(APPEND HYDRA_V8_HINT_ROOTS "/usr/local" "/usr")

if(NOT V8_INCLUDE_DIR AND DEFINED ENV{V8_INCLUDE_DIR} AND NOT "$ENV{V8_INCLUDE_DIR}" STREQUAL "")
  set(V8_INCLUDE_DIR "$ENV{V8_INCLUDE_DIR}" CACHE PATH "V8 include directory" FORCE)
endif()
if(NOT V8_LIBRARIES AND DEFINED ENV{V8_LIBRARIES} AND NOT "$ENV{V8_LIBRARIES}" STREQUAL "")
  set(V8_LIBRARIES "$ENV{V8_LIBRARIES}" CACHE STRING "V8 libraries" FORCE)
endif()

if(HYDRA_V8_AUTODETECT AND (NOT V8_INCLUDE_DIR OR NOT V8_LIBRARIES))
  if(NOT V8_INCLUDE_DIR)
    find_path(HYDRA_V8_INCLUDE_CANDIDATE
      NAMES v8.h
      HINTS ${HYDRA_V8_HINT_ROOTS}
      PATH_SUFFIXES include include/v8
    )
    if(HYDRA_V8_INCLUDE_CANDIDATE)
      set(V8_INCLUDE_DIR "${HYDRA_V8_INCLUDE_CANDIDATE}" CACHE PATH "V8 include directory" FORCE)
      message(STATUS "HydraStack auto-detected V8 include: ${V8_INCLUDE_DIR}")
    endif()
  endif()

  if(NOT V8_LIBRARIES)
    find_library(HYDRA_V8_MONOLITH_LIBRARY
      NAMES v8_monolith
      HINTS ${HYDRA_V8_HINT_ROOTS}
      PATH_SUFFIXES lib lib64
    )
    if(HYDRA_V8_MONOLITH_LIBRARY)
      set(V8_LIBRARIES "${HYDRA_V8_MONOLITH_LIBRARY}" CACHE STRING "V8 libraries" FORCE)
      message(STATUS "HydraStack auto-detected V8 library: ${HYDRA_V8_MONOLITH_LIBRARY}")
    else()
      find_library(HYDRA_V8_LIBRARY
        NAMES v8
        HINTS ${HYDRA_V8_HINT_ROOTS}
        PATH_SUFFIXES lib lib64
      )
      find_library(HYDRA_V8_LIBPLATFORM_LIBRARY
        NAMES v8_libplatform
        HINTS ${HYDRA_V8_HINT_ROOTS}
        PATH_SUFFIXES lib lib64
      )
      if(HYDRA_V8_LIBRARY AND HYDRA_V8_LIBPLATFORM_LIBRARY)
        set(V8_LIBRARIES "${HYDRA_V8_LIBRARY};${HYDRA_V8_LIBPLATFORM_LIBRARY}" CACHE STRING "V8 libraries" FORCE)
        message(STATUS "HydraStack auto-detected V8 libraries: ${V8_LIBRARIES}")
      endif()
    endif()
  endif()
endif()

if(V8_INCLUDE_DIR STREQUAL "...")
  message(FATAL_ERROR "V8_INCLUDE_DIR is still set to placeholder '...'. Provide a real include path.")
endif()

foreach(HYDRA_V8_LIB IN LISTS V8_LIBRARIES)
  if(HYDRA_V8_LIB STREQUAL "...")
    message(FATAL_ERROR "V8_LIBRARIES contains placeholder '...'. Provide real V8 libraries.")
  endif()
endforeach()

if(NOT V8_INCLUDE_DIR OR NOT V8_LIBRARIES)
  message(FATAL_ERROR
    "V8 not configured. Provide -DV8_INCLUDE_DIR/-DV8_LIBRARIES, set V8_INCLUDE_DIR/V8_LIBRARIES env vars, "
    "or install V8 in a standard path and keep HYDRA_V8_AUTODETECT=ON.")
endif()

add_library(hydra_engine
  engine/src/Config.cc
  engine/src/HydraSsrPlugin.cc
  engine/src/HtmlShell.cc
  engine/src/V8IsolatePool.cc
  engine/src/V8Platform.cc
  engine/src/V8SsrRuntime.cc
)
add_library(HydraStack::hydra_engine ALIAS hydra_engine)

set(HYDRA_V8_DEFINITIONS "")
if(V8_COMPILE_DEFINITIONS)
  list(APPEND HYDRA_V8_DEFINITIONS ${V8_COMPILE_DEFINITIONS})
elseif(APPLE AND V8_INCLUDE_DIR MATCHES "/opt/homebrew/.*/v8/include")
  list(APPEND HYDRA_V8_DEFINITIONS V8_COMPRESS_POINTERS V8_ENABLE_SANDBOX)
  message(STATUS "HydraStack detected Homebrew V8; applying defs: V8_COMPRESS_POINTERS;V8_ENABLE_SANDBOX")
endif()

target_include_directories(hydra_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<BUILD_INTERFACE:${V8_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${V8_INCLUDE_DIR}>
)

if(HYDRA_V8_DEFINITIONS)
  target_compile_definitions(hydra_engine PUBLIC ${HYDRA_V8_DEFINITIONS})
endif()

target_link_libraries(hydra_engine
  PUBLIC
    ${HYDRA_DROGON_TARGET}
    ${HYDRA_JSONCPP_TARGET}
    ${V8_LIBRARIES}
)

if(HYDRA_BUILD_DEMO)
  add_executable(hydra_demo
    demo/src/main.cc
    demo/src/controllers/Home.cc
  )

  target_link_libraries(hydra_demo
    PRIVATE
      hydra_engine
  )

  target_compile_definitions(hydra_demo
    PRIVATE
      HYDRA_PUBLIC_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/public\"
  )

  if(HYDRA_BUILD_UI)
    include(cmake/HydraUI.cmake)
    hydra_add_ui_build(hydra_ui_build "${CMAKE_CURRENT_SOURCE_DIR}/ui")
    add_dependencies(hydra_demo hydra_ui_build)
  endif()
endif()

if(BUILD_TESTING)
  add_executable(hydra_config_validation_test
    engine/test/ConfigValidationTest.cc
  )

  target_link_libraries(hydra_config_validation_test
    PRIVATE
      hydra_engine
  )

  add_test(
    NAME hydra_config_validation
    COMMAND hydra_config_validation_test
  )

  if(HYDRA_BUILD_DEMO)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      add_test(
        NAME hydra_ssr_integration
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_ssr_integration.py
          --root
          ${CMAKE_CURRENT_SOURCE_DIR}
          --binary
          $<TARGET_FILE:hydra_demo>
      )

      find_program(HYDRA_AB_EXECUTABLE NAMES ab)
      if(HYDRA_AB_EXECUTABLE)
        add_test(
          NAME hydra_load_regression
          COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_load_regression.py
            --root
            ${CMAKE_CURRENT_SOURCE_DIR}
            --binary
            $<TARGET_FILE:hydra_demo>
            --ab
            ${HYDRA_AB_EXECUTABLE}
        )
      else()
        message(WARNING "apachebench (ab) not found; hydra_load_regression test will be skipped.")
      endif()
    else()
      message(WARNING "Python3 interpreter not found; integration/load tests will be skipped.")
    endif()
  endif()
endif()

install(
  TARGETS hydra_engine
  EXPORT HydraStackTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  DIRECTORY engine/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  EXPORT HydraStackTargets
  FILE HydraStackTargets.cmake
  NAMESPACE HydraStack::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HydraStack
)

configure_package_config_file(
  cmake/HydraConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/HydraStackConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HydraStack
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/HydraStackConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/HydraStackConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/HydraStackConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HydraStack
)
