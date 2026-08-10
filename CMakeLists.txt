cmake_minimum_required(VERSION 3.16)

project(mlTau VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Gaudi REQUIRED)
find_package(k4FWCore REQUIRED)
find_package(EDM4HEP REQUIRED)
find_package(podio REQUIRED)
find_package(nlohmann_json REQUIRED)

find_package(FastJet QUIET)
if(FastJet_FOUND)
  if(TARGET FastJet::fastjet)
    set(MLTAU_FASTJET_TARGET FastJet::fastjet)
  elseif(TARGET fastjet)
    set(MLTAU_FASTJET_TARGET fastjet)
  endif()
endif()
if(NOT MLTAU_FASTJET_TARGET)
  find_path(FASTJET_INCLUDE_DIR
    NAMES fastjet/ClusterSequence.hh
    PATH_SUFFIXES include
  )
  find_library(FASTJET_LIBRARY
    NAMES fastjet
    PATH_SUFFIXES lib lib64
  )
  if(NOT FASTJET_INCLUDE_DIR OR NOT FASTJET_LIBRARY)
    message(FATAL_ERROR
      "Could not find FastJet. Source a Key4hep/spack environment that "
      "provides fastjet, or set FASTJET_INCLUDE_DIR and FASTJET_LIBRARY "
      "explicitly.")
  endif()
  add_library(mlTau::fastjet UNKNOWN IMPORTED)
  set_target_properties(mlTau::fastjet PROPERTIES
    IMPORTED_LOCATION "${FASTJET_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${FASTJET_INCLUDE_DIR}"
  )
  set(MLTAU_FASTJET_TARGET mlTau::fastjet)
endif()

find_package(ONNXRuntime QUIET)
if(ONNXRuntime_FOUND)
  set(MLTAU_ONNXRUNTIME_TARGET ONNXRuntime::ONNXRuntime)
else()
  find_path(ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATH_SUFFIXES include include/onnxruntime include/onnxruntime/core/session
  )
  find_library(ONNXRUNTIME_LIBRARY
    NAMES onnxruntime
    PATH_SUFFIXES lib lib64
  )
  if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR
      "Could not find ONNXRuntime. Source a Key4hep/spack environment that "
      "provides onnxruntime, or set ONNXRUNTIME_INCLUDE_DIR and "
      "ONNXRUNTIME_LIBRARY explicitly.")
  endif()
  add_library(mlTau::onnxruntime UNKNOWN IMPORTED)
  set_target_properties(mlTau::onnxruntime PROPERTIES
    IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
  )
  set(MLTAU_ONNXRUNTIME_TARGET mlTau::onnxruntime)
endif()

gaudi_add_module(mlTauPlugins
  SOURCES
    src/components/mlTauAlg.cpp
    src/components/mlTauSeedBuilder.cpp
  LINK
    Gaudi::GaudiKernel
    k4FWCore::k4FWCore
    EDM4HEP::edm4hep
    podio::podio
    ${MLTAU_FASTJET_TARGET}
    ${MLTAU_ONNXRUNTIME_TARGET}
    nlohmann_json::nlohmann_json
)

target_include_directories(mlTauPlugins
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(MLTAU_RUNTIME_LIBRARY_DIRS "")
foreach(_mltau_runtime_target IN ITEMS ${MLTAU_FASTJET_TARGET} ${MLTAU_ONNXRUNTIME_TARGET})
  foreach(_mltau_runtime_prop IN ITEMS
      IMPORTED_LOCATION
      IMPORTED_LOCATION_RELEASE
      IMPORTED_LOCATION_RELWITHDEBINFO
      IMPORTED_LOCATION_MINSIZEREL
      IMPORTED_LOCATION_DEBUG)
    get_target_property(_mltau_runtime_lib ${_mltau_runtime_target} ${_mltau_runtime_prop})
    if(_mltau_runtime_lib AND NOT _mltau_runtime_lib STREQUAL "_mltau_runtime_lib-NOTFOUND")
      get_filename_component(_mltau_runtime_dir "${_mltau_runtime_lib}" DIRECTORY)
      list(APPEND MLTAU_RUNTIME_LIBRARY_DIRS "${_mltau_runtime_dir}")
      break()
    endif()
  endforeach()
endforeach()
list(REMOVE_DUPLICATES MLTAU_RUNTIME_LIBRARY_DIRS)
string(REPLACE ";" ":" MLTAU_RUNTIME_LIBRARY_PATH "${MLTAU_RUNTIME_LIBRARY_DIRS}")

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup.sh.in
  ${CMAKE_CURRENT_SOURCE_DIR}/setup.sh
  @ONLY
)

install(DIRECTORY options DESTINATION ${CMAKE_INSTALL_DATADIR}/mlTau)
install(DIRECTORY scripts DESTINATION ${CMAKE_INSTALL_DATADIR}/mlTau USE_SOURCE_PERMISSIONS)
install(DIRECTORY config DESTINATION ${CMAKE_INSTALL_DATADIR}/mlTau)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/mlTau)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/setup.sh DESTINATION .)
