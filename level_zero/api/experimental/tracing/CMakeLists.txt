#
# Copyright (C) 2020 Intel Corporation
#
# SPDX-License-Identifier: MIT
#

set(L0_EXPERIMENTAL_TRACING_API
    ${CMAKE_CURRENT_SOURCE_DIR}/zet_tracing.cpp
)

set_property(GLOBAL PROPERTY L0_EXPERIMENTAL_TRACING_API ${L0_EXPERIMENTAL_TRACING_API})
