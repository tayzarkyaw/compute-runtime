#
# Copyright (C) 2017-2020 Intel Corporation
#
# SPDX-License-Identifier: MIT
#

set(IGDRCL_SRCS_linux_dll_tests
)

set(target_name linux_dll_tests)
add_executable(igdrcl_${target_name}
  $<TARGET_OBJECTS:igdrcl_libult>
  $<TARGET_OBJECTS:igdrcl_libult_cs>
  $<TARGET_OBJECTS:mock_gmm>
  ${CMAKE_CURRENT_SOURCE_DIR}/drm_null_device_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/drm_wrap.h
  ${CMAKE_CURRENT_SOURCE_DIR}/linux_tests_configuration.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/main_linux_dll.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/mock_os_layer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/mock_os_layer.h
  ${CMAKE_CURRENT_SOURCE_DIR}/test_mode.h
  ${NEO_SOURCE_DIR}/core/os_interface/linux/sys_calls_linux.cpp
  ${NEO_SOURCE_DIR}/opencl/source/dll/linux/allocator_helper.cpp
  ${NEO_SOURCE_DIR}/opencl/source/dll/linux/drm_neo_create.cpp
  ${NEO_SOURCE_DIR}/opencl/source/dll/linux/options_linux.cpp
  ${NEO_SOURCE_DIR}/opencl/source/dll/linux/os_interface.cpp
  ${NEO_RUNTIME_TESTS_DIR}/aub_stream_mocks/aub_stream_interface_mock.cpp
  ${NEO_RUNTIME_TESTS_DIR}/linux${BRANCH_DIR_SUFFIX}/drm_other_requests.cpp
  ${NEO_RUNTIME_TESTS_DIR}/os_interface/linux/create_drm_memory_manager.cpp
)
if(NEO__LIBVA_FOUND)
  target_sources(igdrcl_${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/va_tests.cpp)
endif()

target_link_libraries(igdrcl_${target_name} ${NEO_MOCKABLE_LIB_NAME} ${NEO_CORE_MOCKABLE_LIB_NAME} ${NEO_MOCKABLE_LIB_NAME} igdrcl_mocks ${IGDRCL_EXTRA_LIBS} gmock-gtest)

set_property(TARGET igdrcl_${target_name} APPEND_STRING PROPERTY COMPILE_FLAGS ${ASAN_FLAGS})
target_include_directories(igdrcl_${target_name} PRIVATE
  ${NEO_SOURCE_DIR}/core/unit_tests/test_macros${BRANCH_DIR_SUFFIX}
  ${NEO_SOURCE_DIR}/opencl/source/dll/linux/devices${BRANCH_DIR_SUFFIX}
  ${NEO_RUNTIME_TESTS_DIR}/gen_common${BRANCH_DIR_SUFFIX}
  ${CMAKE_CURRENT_SOURCE_DIR}
)
add_dependencies(unit_tests igdrcl_${target_name})
add_subdirectories()