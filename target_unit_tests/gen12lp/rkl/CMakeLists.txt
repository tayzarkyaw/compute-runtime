#
# Copyright (C) 2020 Intel Corporation
#
# SPDX-License-Identifier: MIT
#

if(TESTS_RKL)
  set(unit_test_config "rkl/1/2/16")
  include(${NEO_SOURCE_DIR}/cmake/run_ult_target.cmake)
endif()
