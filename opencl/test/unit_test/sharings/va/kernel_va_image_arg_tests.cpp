/*
 * Copyright (C) 2017-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "opencl/source/sharings/va/va_surface.h"
#include "test.h"

#include "fixtures/kernel_arg_fixture.h"
#include "gtest/gtest.h"
#include "mocks/mock_kernel.h"
#include "mocks/mock_program.h"
#include "sharings/va/mock_va_sharing.h"

using namespace NEO;

TEST_F(KernelImageArgTest, givenSharedImageWhenSetArgIsCalledThenReportSharedObjUsage) {
    MockVaSharing vaSharing;
    VASurfaceID vaSurfaceId = 0u;
    vaSharing.updateAcquiredHandle(1u);
    std::unique_ptr<Image> sharedImage(VASurface::createSharedVaSurface(context.get(), &vaSharing.sharingFunctions,
                                                                        CL_MEM_READ_WRITE, &vaSurfaceId, 0, nullptr));

    auto sharedMem = static_cast<cl_mem>(sharedImage.get());
    auto nonSharedMem = static_cast<cl_mem>(image.get());

    EXPECT_FALSE(pKernel->isUsingSharedObjArgs());
    this->pKernel->setArg(0, sizeof(cl_mem *), &nonSharedMem);
    EXPECT_FALSE(pKernel->isUsingSharedObjArgs());

    this->pKernel->setArg(0, sizeof(cl_mem *), &sharedMem);
    EXPECT_TRUE(pKernel->isUsingSharedObjArgs());
}