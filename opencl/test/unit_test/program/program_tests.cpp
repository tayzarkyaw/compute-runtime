/*
 * Copyright (C) 2017-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "opencl/test/unit_test/program/program_tests.h"

#include "shared/source/command_stream/command_stream_receiver_hw.h"
#include "shared/source/compiler_interface/intermediate_representations.h"
#include "shared/source/device_binary_format/elf/elf_decoder.h"
#include "shared/source/device_binary_format/elf/ocl_elf.h"
#include "shared/source/device_binary_format/patchtokens_decoder.h"
#include "shared/source/gmm_helper/gmm_helper.h"
#include "shared/source/helpers/aligned_memory.h"
#include "shared/source/helpers/hash.h"
#include "shared/source/helpers/hw_helper.h"
#include "shared/source/helpers/ptr_math.h"
#include "shared/source/helpers/string.h"
#include "shared/source/memory_manager/allocations_list.h"
#include "shared/source/memory_manager/graphics_allocation.h"
#include "shared/source/memory_manager/surface.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/test/unit_test/device_binary_format/patchtokens_tests.h"
#include "shared/test/unit_test/device_binary_format/zebin_tests.h"
#include "shared/test/unit_test/helpers/debug_manager_state_restore.h"
#include "shared/test/unit_test/mocks/mock_compiler_interface.h"
#include "shared/test/unit_test/utilities/base_object_utils.h"

#include "opencl/source/gtpin/gtpin_notify.h"
#include "opencl/source/helpers/hardware_commands_helper.h"
#include "opencl/source/kernel/kernel.h"
#include "opencl/source/program/create.inl"
#include "opencl/test/unit_test/fixtures/cl_device_fixture.h"
#include "opencl/test/unit_test/fixtures/multi_root_device_fixture.h"
#include "opencl/test/unit_test/global_environment.h"
#include "opencl/test/unit_test/helpers/kernel_binary_helper.h"
#include "opencl/test/unit_test/libult/ult_command_stream_receiver.h"
#include "opencl/test/unit_test/mocks/mock_allocation_properties.h"
#include "opencl/test/unit_test/mocks/mock_graphics_allocation.h"
#include "opencl/test/unit_test/mocks/mock_kernel.h"
#include "opencl/test/unit_test/mocks/mock_platform.h"
#include "opencl/test/unit_test/mocks/mock_program.h"
#include "opencl/test/unit_test/program/program_from_binary.h"
#include "opencl/test/unit_test/program/program_with_source.h"
#include "opencl/test/unit_test/test_macros/test_checks_ocl.h"
#include "test.h"

#include "compiler_options.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace NEO;

void ProgramTests::SetUp() {
    ClDeviceFixture::SetUp();
    cl_device_id device = pClDevice;
    ContextFixture::SetUp(1, &device);
}

void ProgramTests::TearDown() {
    ContextFixture::TearDown();
    ClDeviceFixture::TearDown();
}

void CL_CALLBACK notifyFunc(
    cl_program program,
    void *userData) {
    *((char *)userData) = 'a';
}

std::vector<const char *> BinaryFileNames{
    "CopyBuffer_simd32",
};

std::vector<const char *> SourceFileNames{
    "CopyBuffer_simd16.cl",
};

std::vector<const char *> BinaryForSourceFileNames{
    "CopyBuffer_simd16",
};

std::vector<const char *> KernelNames{
    "CopyBuffer",
};

class NoCompilerInterfaceRootDeviceEnvironment : public RootDeviceEnvironment {
  public:
    NoCompilerInterfaceRootDeviceEnvironment(ExecutionEnvironment &executionEnvironment) : RootDeviceEnvironment(executionEnvironment) {
    }

    CompilerInterface *getCompilerInterface() override {
        return nullptr;
    }
};

class FailingGenBinaryProgram : public MockProgram {
  public:
    FailingGenBinaryProgram(ExecutionEnvironment &executionEnvironment) : MockProgram(executionEnvironment) {}
    cl_int processGenBinary() override { return CL_INVALID_BINARY; }
};

class SucceedingGenBinaryProgram : public MockProgram {
  public:
    SucceedingGenBinaryProgram(ExecutionEnvironment &executionEnvironment) : MockProgram(executionEnvironment) {}
    cl_int processGenBinary() override { return CL_SUCCESS; }
};

TEST_P(ProgramFromBinaryTest, WhenBuildingProgramThenSuccessIsReturned) {
    cl_device_id device = pClDevice;
    retVal = pProgram->build(
        1,
        &device,
        nullptr,
        nullptr,
        nullptr,
        false);

    EXPECT_EQ(CL_SUCCESS, retVal);
}

TEST_P(ProgramFromBinaryTest, WhenGettingProgramContextInfoThenCorrectContextIsReturned) {
    cl_context contextRet = reinterpret_cast<cl_context>(static_cast<uintptr_t>(0xdeaddead));
    size_t paramValueSizeRet = 0;

    retVal = pProgram->getInfo(
        CL_PROGRAM_CONTEXT,
        sizeof(cl_context),
        &contextRet,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(pContext, contextRet);
    EXPECT_EQ(sizeof(cl_context), paramValueSizeRet);
}

TEST_P(ProgramFromBinaryTest, GivenNonNullParamValueWhenGettingProgramBinaryInfoThenCorrectBinaryIsReturned) {
    size_t paramValueSize = sizeof(unsigned char **);
    size_t paramValueSizeRet = 0;
    auto testBinary = std::make_unique<char[]>(knownSourceSize);

    retVal = pProgram->getInfo(
        CL_PROGRAM_BINARIES,
        paramValueSize,
        &testBinary,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(paramValueSize, paramValueSizeRet);
    EXPECT_STREQ((const char *)knownSource.get(), (const char *)testBinary.get());
}

TEST_P(ProgramFromBinaryTest, GivenNullParamValueWhenGettingProgramBinaryInfoThenSuccessIsReturned) {
    size_t paramValueSize = sizeof(unsigned char **);
    size_t paramValueSizeRet = 0;

    retVal = pProgram->getInfo(
        CL_PROGRAM_BINARIES,
        0,
        nullptr,
        &paramValueSizeRet);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(paramValueSize, paramValueSizeRet);
}

TEST_P(ProgramFromBinaryTest, GivenNonNullParamValueAndParamValueSizeZeroWhenGettingProgramBinaryInfoThenInvalidValueErrorIsReturned) {
    size_t paramValueSizeRet = 0;
    auto testBinary = std::make_unique<char[]>(knownSourceSize);

    retVal = pProgram->getInfo(
        CL_PROGRAM_BINARIES,
        0,
        &testBinary,
        &paramValueSizeRet);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);
}

TEST_P(ProgramFromBinaryTest, GivenInvalidParametersWhenGettingProgramInfoThenValueSizeRetIsNotUpdated) {
    size_t paramValueSizeRet = 0x1234;
    auto testBinary = std::make_unique<char[]>(knownSourceSize);

    retVal = pProgram->getInfo(
        CL_PROGRAM_BINARIES,
        0,
        &testBinary,
        &paramValueSizeRet);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    EXPECT_EQ(0x1234u, paramValueSizeRet);
}

TEST_P(ProgramFromBinaryTest, GivenInvalidParamWhenGettingProgramBinaryInfoThenInvalidValueErrorIsReturned) {
    size_t paramValueSizeRet = 0;
    auto testBinary = std::make_unique<char[]>(knownSourceSize);

    retVal = pProgram->getInfo(
        CL_PROGRAM_BUILD_STATUS,
        0,
        nullptr,
        &paramValueSizeRet);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);
}

TEST_P(ProgramFromBinaryTest, WhenGettingBinarySizesThenCorrectSizesAreReturned) {
    size_t paramValueSize = sizeof(size_t *);
    size_t paramValue[1];
    size_t paramValueSizeRet = 0;

    retVal = pProgram->getInfo(
        CL_PROGRAM_BINARY_SIZES,
        paramValueSize,
        paramValue,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(knownSourceSize, paramValue[0]);
    EXPECT_EQ(paramValueSize, paramValueSizeRet);
}

TEST_P(ProgramFromBinaryTest, GivenProgramWithOneKernelWhenGettingNumKernelsThenOneIsReturned) {
    size_t paramValue = 0;
    size_t paramValueSize = sizeof(paramValue);
    size_t paramValueSizeRet = 0;
    cl_device_id device = pClDevice;

    retVal = pProgram->build(
        1,
        &device,
        nullptr,
        nullptr,
        nullptr,
        false);
    ASSERT_EQ(CL_SUCCESS, retVal);

    retVal = pProgram->getInfo(
        CL_PROGRAM_NUM_KERNELS,
        paramValueSize,
        &paramValue,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(1u, paramValue);
    EXPECT_EQ(paramValueSize, paramValueSizeRet);
}

TEST_P(ProgramFromBinaryTest, GivenProgramWithNoExecutableCodeWhenGettingNumKernelsThenInvalidProgramExecutableErrorIsReturned) {
    size_t paramValue = 0;
    size_t paramValueSize = sizeof(paramValue);
    size_t paramValueSizeRet = 0;
    cl_device_id device = pClDevice;

    CreateProgramFromBinary(pContext, &device, BinaryFileName);
    MockProgram *p = pProgram;
    p->setBuildStatus(CL_BUILD_NONE);

    retVal = pProgram->getInfo(
        CL_PROGRAM_NUM_KERNELS,
        paramValueSize,
        &paramValue,
        &paramValueSizeRet);
    EXPECT_EQ(CL_INVALID_PROGRAM_EXECUTABLE, retVal);
}

TEST_P(ProgramFromBinaryTest, WhenGettingKernelNamesThenCorrectNameIsReturned) {
    size_t paramValueSize = sizeof(size_t *);
    size_t paramValueSizeRet = 0;
    cl_device_id device = pClDevice;

    retVal = pProgram->build(
        1,
        &device,
        nullptr,
        nullptr,
        nullptr,
        false);
    ASSERT_EQ(CL_SUCCESS, retVal);

    // get info successfully about required sizes for kernel names
    retVal = pProgram->getInfo(
        CL_PROGRAM_KERNEL_NAMES,
        0,
        nullptr,
        &paramValueSizeRet);
    ASSERT_EQ(CL_SUCCESS, retVal);
    ASSERT_NE(0u, paramValueSizeRet);

    // get info successfully about kernel names
    auto paramValue = std::make_unique<char[]>(paramValueSizeRet);
    paramValueSize = paramValueSizeRet;
    ASSERT_NE(paramValue, nullptr);

    size_t expectedKernelsStringSize = strlen(KernelName) + 1;
    retVal = pProgram->getInfo(
        CL_PROGRAM_KERNEL_NAMES,
        paramValueSize,
        paramValue.get(),
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_STREQ(KernelName, (char *)paramValue.get());
    EXPECT_EQ(expectedKernelsStringSize, paramValueSizeRet);
}

TEST_P(ProgramFromBinaryTest, GivenProgramWithNoExecutableCodeWhenGettingKernelNamesThenInvalidProgramExecutableErrorIsReturned) {
    size_t paramValueSize = sizeof(size_t *);
    size_t paramValueSizeRet = 0;
    cl_device_id device = pClDevice;

    CreateProgramFromBinary(pContext, &device, BinaryFileName);
    MockProgram *p = pProgram;
    p->setBuildStatus(CL_BUILD_NONE);

    retVal = pProgram->getInfo(
        CL_PROGRAM_KERNEL_NAMES,
        paramValueSize,
        nullptr,
        &paramValueSizeRet);
    EXPECT_EQ(CL_INVALID_PROGRAM_EXECUTABLE, retVal);
}

TEST_P(ProgramFromBinaryTest, WhenGettingProgramScopeGlobalCtorsAndDtorsPresentInfoThenCorrectValueIsReturned) {
    cl_uint paramRet = 0;
    cl_uint expectedParam = CL_FALSE;
    size_t paramSizeRet = 0;

    retVal = pProgram->getInfo(
        CL_PROGRAM_SCOPE_GLOBAL_CTORS_PRESENT,
        sizeof(cl_uint),
        &paramRet,
        &paramSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(sizeof(cl_uint), paramSizeRet);
    EXPECT_EQ(expectedParam, paramRet);

    retVal = pProgram->getInfo(
        CL_PROGRAM_SCOPE_GLOBAL_DTORS_PRESENT,
        sizeof(cl_uint),
        &paramRet,
        &paramSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(sizeof(cl_uint), paramSizeRet);
    EXPECT_EQ(expectedParam, paramRet);
}

TEST_P(ProgramFromBinaryTest, GivenInvalidDeviceWhenGettingBuildStatusThenInvalidDeviceErrorIsReturned) {
    cl_build_status buildStatus = 0;
    size_t paramValueSize = sizeof(buildStatus);
    size_t paramValueSizeRet = 0;

    size_t invalidDevice = 0xdeadbee0;
    retVal = pProgram->getBuildInfo(
        reinterpret_cast<ClDevice *>(invalidDevice),
        CL_PROGRAM_BUILD_STATUS,
        paramValueSize,
        &buildStatus,
        &paramValueSizeRet);
    EXPECT_EQ(CL_INVALID_DEVICE, retVal);
}

TEST_P(ProgramFromBinaryTest, GivenCorruptedDeviceWhenGettingBuildStatusThenInvalidDiveErrorIsReturned) {
    cl_build_status buildStatus = 0;
    size_t paramValueSize = sizeof(buildStatus);
    size_t paramValueSizeRet = 0;

    cl_device_id device = pClDevice;
    CreateProgramFromBinary(pContext, &device, BinaryFileName);
    MockProgram *p = pProgram;
    p->setDevice(&pClDevice->getDevice());

    retVal = pProgram->getBuildInfo(
        reinterpret_cast<ClDevice *>(pContext),
        CL_PROGRAM_BUILD_STATUS,
        paramValueSize,
        &buildStatus,
        &paramValueSizeRet);
    EXPECT_EQ(CL_INVALID_DEVICE, retVal);
}

TEST_P(ProgramFromBinaryTest, GivenNullDeviceWhenGettingBuildStatusThenBuildNoneIsReturned) {
    cl_device_id device = pClDevice;
    cl_build_status buildStatus = 0;
    size_t paramValueSize = sizeof(buildStatus);
    size_t paramValueSizeRet = 0;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_STATUS,
        paramValueSize,
        &buildStatus,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(paramValueSize, paramValueSizeRet);
    EXPECT_EQ(CL_BUILD_NONE, buildStatus);
}

TEST_P(ProgramFromBinaryTest, GivenInvalidParametersWhenGettingBuildInfoThenValueSizeRetIsNotUpdated) {
    cl_device_id device = pClDevice;
    cl_build_status buildStatus = 0;
    size_t paramValueSize = sizeof(buildStatus);
    size_t paramValueSizeRet = 0x1234;

    retVal = pProgram->getBuildInfo(
        device,
        0,
        paramValueSize,
        &buildStatus,
        &paramValueSizeRet);

    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    EXPECT_EQ(0x1234u, paramValueSizeRet);
}

TEST_P(ProgramFromBinaryTest, GivenDefaultDeviceWhenGettingBuildOptionsThenBuildOptionsAreEmpty) {
    cl_device_id device = pClDevice;
    size_t paramValueSizeRet = 0u;
    size_t paramValueSize = 0u;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_OPTIONS,
        0,
        nullptr,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(paramValueSizeRet, 0u);

    auto paramValue = std::make_unique<char[]>(paramValueSizeRet);
    paramValueSize = paramValueSizeRet;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_OPTIONS,
        paramValueSize,
        paramValue.get(),
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_STREQ("", (char *)paramValue.get());
}

TEST_P(ProgramFromBinaryTest, GivenDefaultDeviceWhenGettingLogThenLogEmpty) {
    cl_device_id device = pClDevice;
    size_t paramValueSizeRet = 0u;
    size_t paramValueSize = 0u;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_LOG,
        0,
        nullptr,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(paramValueSizeRet, 0u);

    auto paramValue = std::make_unique<char[]>(paramValueSizeRet);
    paramValueSize = paramValueSizeRet;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_LOG,
        paramValueSize,
        paramValue.get(),
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_STREQ("", (char *)paramValue.get());
}

TEST_P(ProgramFromBinaryTest, GivenLogEntriesWhenGetBuildLogThenLogIsApended) {
    cl_device_id device = pClDevice;
    size_t paramValueSizeRet = 0u;
    size_t paramValueSize = 0u;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_LOG,
        0,
        nullptr,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(paramValueSizeRet, 0u);

    auto paramValue = std::make_unique<char[]>(paramValueSizeRet);
    paramValueSize = paramValueSizeRet;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_LOG,
        paramValueSize,
        paramValue.get(),
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_STREQ("", (char *)paramValue.get());

    // Add more text to the log
    pProgram->updateBuildLog(pClDevice->getRootDeviceIndex(), "testing", 8);
    pProgram->updateBuildLog(pClDevice->getRootDeviceIndex(), "several", 8);

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_LOG,
        0,
        nullptr,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_GE(paramValueSizeRet, 16u);
    paramValue = std::make_unique<char[]>(paramValueSizeRet);

    paramValueSize = paramValueSizeRet;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_LOG,
        paramValueSize,
        paramValue.get(),
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);

    EXPECT_NE(nullptr, strstr(paramValue.get(), "testing"));

    const char *paramValueContinued = strstr(paramValue.get(), "testing") + 7;
    ASSERT_NE(nullptr, strstr(paramValueContinued, "several"));
}

TEST_P(ProgramFromBinaryTest, GivenNullParamValueWhenGettingProgramBinaryTypeThenParamValueSizeIsReturned) {
    cl_device_id device = pClDevice;
    size_t paramValueSizeRet = 0u;
    size_t paramValueSize = 0u;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BINARY_TYPE,
        paramValueSize,
        nullptr,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(paramValueSizeRet, 0u);
}

TEST_P(ProgramFromBinaryTest, WhenGettingProgramBinaryTypeThenCorrectProgramTypeIsReturned) {
    cl_device_id device = pClDevice;
    cl_program_binary_type programType = 0;
    char *paramValue = (char *)&programType;
    size_t paramValueSizeRet = 0u;
    size_t paramValueSize = 0u;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BINARY_TYPE,
        paramValueSize,
        nullptr,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(paramValueSizeRet, 0u);

    paramValueSize = paramValueSizeRet;
    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BINARY_TYPE,
        paramValueSize,
        paramValue,
        &paramValueSizeRet);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ((cl_program_binary_type)CL_PROGRAM_BINARY_TYPE_EXECUTABLE, programType);
}

TEST_P(ProgramFromBinaryTest, GivenInvalidParamWhenGettingBuildInfoThenInvalidValueErrorIsReturned) {
    cl_device_id device = pClDevice;
    size_t paramValueSizeRet = 0u;

    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_KERNEL_NAMES,
        0,
        nullptr,
        &paramValueSizeRet);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);
}

TEST_P(ProgramFromBinaryTest, GivenGlobalVariableTotalSizeSetWhenGettingBuildGlobalVariableTotalSizeThenCorrectSizeIsReturned) {
    cl_device_id device = pClDevice;
    size_t globalVarSize = 22;
    size_t paramValueSize = sizeof(globalVarSize);
    size_t paramValueSizeRet = 0;
    char *paramValue = (char *)&globalVarSize;

    // get build info as is
    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_GLOBAL_VARIABLE_TOTAL_SIZE,
        paramValueSize,
        paramValue,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(paramValueSizeRet, sizeof(globalVarSize));
    EXPECT_EQ(globalVarSize, 0u);

    // Set GlobalVariableTotalSize as 1024
    CreateProgramFromBinary(pContext, &device, BinaryFileName);
    MockProgram *p = pProgram;
    ProgramInfo programInfo;

    char constantData[1024] = {};
    programInfo.globalVariables.initData = constantData;
    programInfo.globalVariables.size = sizeof(constantData);
    p->processProgramInfo(programInfo);

    // get build info once again
    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_GLOBAL_VARIABLE_TOTAL_SIZE,
        paramValueSize,
        paramValue,
        &paramValueSizeRet);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(paramValueSizeRet, sizeof(globalVarSize));
    if (castToObject<ClDevice>(pClDevice)->areOcl21FeaturesEnabled()) {
        EXPECT_EQ(globalVarSize, 1024u);
    } else {
        EXPECT_EQ(globalVarSize, 0u);
    }
}

TEST_P(ProgramFromBinaryTest, givenProgramWhenItIsBeingBuildThenItContainsGraphicsAllocationInKernelInfo) {
    cl_device_id device = pClDevice;
    pProgram->build(1, &device, nullptr, nullptr, nullptr, true);
    auto kernelInfo = pProgram->getKernelInfo(size_t(0));

    auto graphicsAllocation = kernelInfo->getGraphicsAllocation();
    ASSERT_NE(nullptr, graphicsAllocation);
    EXPECT_TRUE(graphicsAllocation->is32BitAllocation());
    EXPECT_EQ(graphicsAllocation->getUnderlyingBufferSize(), kernelInfo->heapInfo.KernelHeapSize);

    auto kernelIsa = graphicsAllocation->getUnderlyingBuffer();
    EXPECT_NE(kernelInfo->heapInfo.pKernelHeap, kernelIsa);
    EXPECT_EQ(0, memcmp(kernelIsa, kernelInfo->heapInfo.pKernelHeap, kernelInfo->heapInfo.KernelHeapSize));
    auto rootDeviceIndex = graphicsAllocation->getRootDeviceIndex();
    EXPECT_EQ(GmmHelper::decanonize(graphicsAllocation->getGpuBaseAddress()), pProgram->getDevice().getMemoryManager()->getInternalHeapBaseAddress(rootDeviceIndex, graphicsAllocation->isAllocatedInLocalMemoryPool()));
}

TEST_P(ProgramFromBinaryTest, whenProgramIsBeingRebuildThenOutdatedGlobalBuffersAreFreed) {
    cl_device_id device = pClDevice;
    pProgram->build(1, &device, nullptr, nullptr, nullptr, true);
    EXPECT_EQ(nullptr, pProgram->buildInfos[pClDevice->getRootDeviceIndex()].constantSurface);
    EXPECT_EQ(nullptr, pProgram->buildInfos[pClDevice->getRootDeviceIndex()].globalSurface);

    pProgram->buildInfos[pClDevice->getRootDeviceIndex()].constantSurface = new MockGraphicsAllocation();
    pProgram->processGenBinary();
    EXPECT_EQ(nullptr, pProgram->buildInfos[pClDevice->getRootDeviceIndex()].constantSurface);
    EXPECT_EQ(nullptr, pProgram->buildInfos[pClDevice->getRootDeviceIndex()].globalSurface);

    pProgram->buildInfos[pClDevice->getRootDeviceIndex()].globalSurface = new MockGraphicsAllocation();
    pProgram->processGenBinary();
    EXPECT_EQ(nullptr, pProgram->buildInfos[pClDevice->getRootDeviceIndex()].constantSurface);
    EXPECT_EQ(nullptr, pProgram->buildInfos[pClDevice->getRootDeviceIndex()].globalSurface);
}

TEST_P(ProgramFromBinaryTest, givenProgramWhenCleanKernelInfoIsCalledThenKernelAllocationIsFreed) {
    cl_device_id device = pClDevice;
    pProgram->build(1, &device, nullptr, nullptr, nullptr, true);
    EXPECT_EQ(1u, pProgram->getNumKernels());
    pProgram->cleanCurrentKernelInfo();
    EXPECT_EQ(0u, pProgram->getNumKernels());
}

HWTEST_P(ProgramFromBinaryTest, givenProgramWhenCleanCurrentKernelInfoIsCalledButGpuIsNotYetDoneThenKernelAllocationIsPutOnDeferredFreeListAndCsrRegistersCacheFlush) {
    cl_device_id device = pClDevice;
    auto &csr = pDevice->getGpgpuCommandStreamReceiver();
    EXPECT_TRUE(csr.getTemporaryAllocations().peekIsEmpty());
    pProgram->build(1, &device, nullptr, nullptr, nullptr, true);
    auto kernelAllocation = pProgram->getKernelInfo(size_t(0))->getGraphicsAllocation();
    kernelAllocation->updateTaskCount(100, csr.getOsContext().getContextId());
    *csr.getTagAddress() = 0;
    pProgram->cleanCurrentKernelInfo();
    EXPECT_FALSE(csr.getTemporaryAllocations().peekIsEmpty());
    EXPECT_EQ(csr.getTemporaryAllocations().peekHead(), kernelAllocation);
    EXPECT_TRUE(this->pDevice->getUltCommandStreamReceiver<FamilyType>().requiresInstructionCacheFlush);
}

HWTEST_P(ProgramFromBinaryTest, givenIsaAllocationUsedByMultipleCsrsWhenItIsDeletedItRegistersCacheFlushInEveryCsrThatUsedIt) {
    auto &csr0 = this->pDevice->getUltCommandStreamReceiverFromIndex<FamilyType>(0u);
    auto &csr1 = this->pDevice->getUltCommandStreamReceiverFromIndex<FamilyType>(1u);

    cl_device_id device = pClDevice;

    pProgram->build(1, &device, nullptr, nullptr, nullptr, true);

    auto kernelAllocation = pProgram->getKernelInfo(size_t(0))->getGraphicsAllocation();

    csr0.makeResident(*kernelAllocation);
    csr1.makeResident(*kernelAllocation);

    csr0.processResidency(csr0.getResidencyAllocations(), 0u);
    csr1.processResidency(csr1.getResidencyAllocations(), 0u);

    csr0.makeNonResident(*kernelAllocation);
    csr1.makeNonResident(*kernelAllocation);

    EXPECT_FALSE(csr0.requiresInstructionCacheFlush);
    EXPECT_FALSE(csr1.requiresInstructionCacheFlush);

    pProgram->cleanCurrentKernelInfo();
    EXPECT_TRUE(csr0.requiresInstructionCacheFlush);
    EXPECT_TRUE(csr1.requiresInstructionCacheFlush);
}

TEST_P(ProgramFromSourceTest, GivenSpecificParamatersWhenBuildingProgramThenSuccessOrCorrectErrorCodeIsReturned) {
    KernelBinaryHelper kbHelper(BinaryFileName, true);
    auto device = pPlatform->getClDevice(0);

    cl_device_id deviceList = {0};
    char data[4] = {0};

    cl_device_id usedDevice = pPlatform->getClDevice(0);

    CreateProgramWithSource(
        pContext,
        &usedDevice,
        SourceFileName);

    // Order of following microtests is important - do not change.
    // Add new microtests at end.

    auto pMockProgram = pProgram;

    // invalid build parameters: combinations of numDevices & deviceList
    retVal = pProgram->build(1, nullptr, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    retVal = pProgram->build(0, &deviceList, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    // invalid build parameters: combinations of funcNotify & userData
    retVal = pProgram->build(0, nullptr, nullptr, nullptr, &data[0], false);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    // invalid build parameters: invalid content of deviceList
    retVal = pProgram->build(1, &deviceList, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_INVALID_DEVICE, retVal);

    // fail build - another build is already in progress
    pMockProgram->setBuildStatus(CL_BUILD_IN_PROGRESS);
    retVal = pProgram->build(0, nullptr, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_INVALID_OPERATION, retVal);
    pMockProgram->setBuildStatus(CL_BUILD_NONE);

    // fail build - CompilerInterface cannot be obtained

    auto executionEnvironment = device->getExecutionEnvironment();
    std::unique_ptr<RootDeviceEnvironment> rootDeviceEnvironment = std::make_unique<NoCompilerInterfaceRootDeviceEnvironment>(*executionEnvironment);
    std::swap(rootDeviceEnvironment, executionEnvironment->rootDeviceEnvironments[device->getRootDeviceIndex()]);
    auto p2 = std::make_unique<MockProgram>(*executionEnvironment);
    p2->setDevice(&device->getDevice());
    retVal = p2->build(0, nullptr, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_OUT_OF_HOST_MEMORY, retVal);
    p2.reset(nullptr);
    std::swap(rootDeviceEnvironment, executionEnvironment->rootDeviceEnvironments[device->getRootDeviceIndex()]);

    // fail build - any build error (here caused by specifying unrecognized option)
    retVal = pProgram->build(0, nullptr, "-invalid-option", nullptr, nullptr, false);
    EXPECT_EQ(CL_BUILD_PROGRAM_FAILURE, retVal);

    // fail build - linked code is corrupted and cannot be postprocessed
    auto p3 = std::make_unique<FailingGenBinaryProgram>(*executionEnvironment);
    p3->setDevice(&device->getDevice());
    std::string testFile;
    size_t sourceSize;
    testFile.append(clFiles);
    testFile.append("CopyBuffer_simd16.cl"); // source file
    auto pSourceBuffer = loadDataFromFile(testFile.c_str(), sourceSize);
    EXPECT_NE(0u, sourceSize);
    EXPECT_NE(nullptr, pSourceBuffer);
    p3->sourceCode = pSourceBuffer.get();
    p3->createdFrom = Program::CreatedFrom::SOURCE;
    retVal = p3->build(0, nullptr, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_INVALID_BINARY, retVal);
    p3.reset(nullptr);

    // build successfully without notifyFunc - build kernel and write it to Kernel Cache
    pMockProgram->clearOptions();
    retVal = pProgram->build(0, nullptr, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_TRUE(CompilerOptions::contains(pProgram->getInternalOptions(), pPlatform->getClDevice(0)->peekCompilerExtensions())) << pProgram->getInternalOptions();

    // get build log
    size_t param_value_size_ret = 0u;
    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_LOG,
        0,
        nullptr,
        &param_value_size_ret);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(param_value_size_ret, 0u);

    // get build log when the log does not exist
    pMockProgram->clearLog(device->getRootDeviceIndex());
    retVal = pProgram->getBuildInfo(
        device,
        CL_PROGRAM_BUILD_LOG,
        0,
        nullptr,
        &param_value_size_ret);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(param_value_size_ret, 0u);

    // build successfully without notifyFunc - build kernel but do not write it to Kernel Cache (kernel is already in the Cache)
    pMockProgram->setBuildStatus(CL_BUILD_NONE);
    retVal = pProgram->build(0, nullptr, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // build successfully with notifyFunc - duplicate build (kernel already built), do not build and just take it
    retVal = pProgram->build(0, nullptr, nullptr, notifyFunc, &data[0], false);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ('a', data[0]);

    // build successfully without notifyFunc - kernel is already in Kernel Cache, do not build and take it from Cache
    retVal = pProgram->build(0, nullptr, nullptr, nullptr, nullptr, true);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // fail build - code to be build does not exist
    pMockProgram->sourceCode = ""; // set source code as non-existent (invalid)
    pMockProgram->createdFrom = Program::CreatedFrom::SOURCE;
    pMockProgram->setBuildStatus(CL_BUILD_NONE);
    pMockProgram->setCreatedFromBinary(false);
    retVal = pProgram->build(0, nullptr, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_INVALID_PROGRAM, retVal);
}

TEST_P(ProgramFromSourceTest, CreateWithSource_Build_Options_Duplicate) {
    KernelBinaryHelper kbHelper(BinaryFileName, false);

    retVal = pProgram->build(0, nullptr, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);

    retVal = pProgram->build(0, nullptr, CompilerOptions::fastRelaxedMath.data(), nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);

    retVal = pProgram->build(0, nullptr, CompilerOptions::fastRelaxedMath.data(), nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);

    retVal = pProgram->build(0, nullptr, CompilerOptions::finiteMathOnly.data(), nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);

    retVal = pProgram->build(0, nullptr, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);
}

TEST_P(ProgramFromSourceTest, WhenBuildingProgramThenFeaturesOptionIsAdded) {
    auto featuresOption = static_cast<ClDevice *>(devices[0])->peekCompilerFeatures();
    EXPECT_THAT(pProgram->getInternalOptions(), testing::Not(testing::HasSubstr(featuresOption)));

    retVal = pProgram->build(1, devices, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_THAT(pProgram->getInternalOptions(), testing::HasSubstr(featuresOption));
}

TEST_P(ProgramFromSourceTest, WhenBuildingProgramThenFeaturesOptionIsAddedOnlyOnce) {
    retVal = pProgram->build(1, devices, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);
    retVal = pProgram->build(1, devices, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);

    auto expectedFeaturesOption = static_cast<ClDevice *>(devices[0])->peekCompilerFeatures();
    auto &internalOptions = pProgram->getInternalOptions();
    auto pos = internalOptions.find(expectedFeaturesOption);
    EXPECT_NE(std::string::npos, pos);

    pos = internalOptions.find(expectedFeaturesOption, pos + 1);
    EXPECT_EQ(std::string::npos, pos);
}

TEST_P(ProgramFromSourceTest, WhenCompilingProgramThenFeaturesOptionIsAdded) {
    auto pCompilerInterface = new MockCompilerInterfaceCaptureBuildOptions();
    auto pClDevice = static_cast<ClDevice *>(devices[0]);
    pClDevice->getExecutionEnvironment()->rootDeviceEnvironments[pClDevice->getRootDeviceIndex()]->compilerInterface.reset(pCompilerInterface);
    auto featuresOption = pClDevice->peekCompilerFeatures();
    EXPECT_THAT(pCompilerInterface->buildInternalOptions, testing::Not(testing::HasSubstr(featuresOption)));

    retVal = pProgram->compile(1, devices, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_THAT(pCompilerInterface->buildInternalOptions, testing::HasSubstr(featuresOption));
}

class Callback {
  public:
    Callback() {
        this->oldCallback = MemoryManagement::deleteCallback;
        MemoryManagement::deleteCallback = thisCallback;
    }
    ~Callback() {
        MemoryManagement::deleteCallback = this->oldCallback;
    }
    static void watch(const void *p) {
        watchList[p] = 0u;
    }
    static void unwatch(const void *p) {
        EXPECT_GT(watchList[p], 0u);
        watchList.erase(p);
    }

  private:
    void (*oldCallback)(void *);
    static void thisCallback(void *p) {
        if (watchList.find(p) != watchList.end())
            watchList[p]++;
    }
    static std::map<const void *, uint32_t> watchList;
};

std::map<const void *, uint32_t> Callback::watchList;

TEST_P(ProgramFromSourceTest, GivenDifferentCommpilerOptionsWhenBuildingProgramThenKernelHashesAreDifferent) {
    KernelBinaryHelper kbHelper(BinaryFileName, true);

    cl_device_id usedDevice = pPlatform->getClDevice(0);
    CreateProgramWithSource(
        pContext,
        &usedDevice,
        SourceFileName);

    Callback callback;

    retVal = pProgram->build(0, nullptr, nullptr, nullptr, nullptr, true);
    EXPECT_EQ(CL_SUCCESS, retVal);
    auto hash1 = pProgram->getCachedFileName();
    auto kernel1 = pProgram->getKernelInfo("CopyBuffer");
    Callback::watch(kernel1);
    EXPECT_NE(nullptr, kernel1);

    retVal = pProgram->build(0, nullptr, CompilerOptions::fastRelaxedMath.data(), nullptr, nullptr, true);
    EXPECT_EQ(CL_SUCCESS, retVal);
    auto hash2 = pProgram->getCachedFileName();
    auto kernel2 = pProgram->getKernelInfo("CopyBuffer");
    EXPECT_NE(nullptr, kernel2);
    EXPECT_NE(hash1, hash2);
    Callback::unwatch(kernel1);
    Callback::watch(kernel2);

    retVal = pProgram->build(0, nullptr, CompilerOptions::finiteMathOnly.data(), nullptr, nullptr, true);
    EXPECT_EQ(CL_SUCCESS, retVal);
    auto hash3 = pProgram->getCachedFileName();
    auto kernel3 = pProgram->getKernelInfo("CopyBuffer");
    EXPECT_NE(nullptr, kernel3);
    EXPECT_NE(hash1, hash3);
    EXPECT_NE(hash2, hash3);
    Callback::unwatch(kernel2);
    Callback::watch(kernel3);

    retVal = pProgram->build(0, nullptr, nullptr, nullptr, nullptr, true);
    EXPECT_EQ(CL_SUCCESS, retVal);
    auto hash4 = pProgram->getCachedFileName();
    auto kernel4 = pProgram->getKernelInfo("CopyBuffer");
    EXPECT_NE(nullptr, kernel4);
    EXPECT_EQ(hash1, hash4);
    Callback::unwatch(kernel3);
}

TEST_P(ProgramFromSourceTest, GivenEmptyProgramWhenCreatingProgramThenInvalidValueErrorIsReturned) {
    auto p = Program::create(pContext, 0, nullptr, nullptr, retVal);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    EXPECT_EQ(nullptr, p);
    delete p;
}

TEST_P(ProgramFromSourceTest, GivenSpecificParamatersWhenCompilingProgramThenSuccessOrCorrectErrorCodeIsReturned) {

    cl_device_id usedDevice = pPlatform->getClDevice(0);
    CreateProgramWithSource(
        pContext,
        &usedDevice,
        SourceFileName);

    auto *p = (MockProgram *)pProgram;

    cl_device_id deviceList = {0};
    cl_program inputHeaders;
    const char *headerIncludeNames = "";
    cl_program nullprogram = nullptr;
    cl_program invprogram = (cl_program)pContext;
    char data[4];

    // Order of following microtests is important - do not change.
    // Add new microtests at end.

    // invalid compile parameters: combinations of numDevices & deviceList
    retVal = pProgram->compile(1, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    retVal = pProgram->compile(0, &deviceList, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    // invalid compile parameters: combinations of numInputHeaders==0 & inputHeaders & headerIncludeNames
    retVal = pProgram->compile(0, nullptr, nullptr, 0, &inputHeaders, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    retVal = pProgram->compile(0, nullptr, nullptr, 0, nullptr, &headerIncludeNames, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    // invalid compile parameters: combinations of numInputHeaders!=0 & inputHeaders & headerIncludeNames
    retVal = pProgram->compile(0, nullptr, nullptr, 1, &inputHeaders, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    retVal = pProgram->compile(0, nullptr, nullptr, 1, nullptr, &headerIncludeNames, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    // invalid compile parameters: combinations of funcNotify & userData with valid numInputHeaders!=0 & inputHeaders & headerIncludeNames
    retVal = pProgram->compile(0, nullptr, nullptr, 1, &inputHeaders, &headerIncludeNames, nullptr, &data[0]);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    // invalid compile parameters: invalid content of deviceList
    retVal = pProgram->compile(1, &deviceList, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_DEVICE, retVal);

    // fail compilation - another compilation is already in progress
    p->setBuildStatus(CL_BUILD_IN_PROGRESS);
    retVal = pProgram->compile(0, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_OPERATION, retVal);
    p->setBuildStatus(CL_BUILD_NONE);

    // invalid compile parameters: invalid header Program object==nullptr
    retVal = pProgram->compile(0, nullptr, nullptr, 1, &nullprogram, &headerIncludeNames, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_PROGRAM, retVal);

    // invalid compile parameters: invalid header Program object==non Program object
    retVal = pProgram->compile(0, nullptr, nullptr, 1, &invprogram, &headerIncludeNames, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_PROGRAM, retVal);

    // compile successfully kernel with header
    std::string testFile;
    size_t sourceSize;
    Program *p3; // header Program object
    testFile.append(clFiles);
    testFile.append("CopyBuffer_simd16.cl"); // header source file
    auto pSourceBuffer = loadDataFromFile(testFile.c_str(), sourceSize);
    EXPECT_NE(0u, sourceSize);
    EXPECT_NE(nullptr, pSourceBuffer);
    const char *sources[1] = {pSourceBuffer.get()};
    p3 = Program::create<MockProgram>(pContext, 1, sources, &sourceSize, retVal);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(nullptr, p3);
    inputHeaders = p3;
    retVal = pProgram->compile(0, nullptr, nullptr, 1, &inputHeaders, &headerIncludeNames, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // fail compilation of kernel with header - header is invalid
    p = (MockProgram *)p3;
    p->sourceCode = ""; // set header source code as non-existent (invalid)
    retVal = pProgram->compile(0, nullptr, nullptr, 1, &inputHeaders, &headerIncludeNames, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_PROGRAM, retVal);
    delete p3;

    // fail compilation - CompilerInterface cannot be obtained
    auto device = pContext->getDevice(0);
    auto executionEnvironment = device->getExecutionEnvironment();
    std::unique_ptr<RootDeviceEnvironment> rootDeviceEnvironment = std::make_unique<NoCompilerInterfaceRootDeviceEnvironment>(*executionEnvironment);
    std::swap(rootDeviceEnvironment, executionEnvironment->rootDeviceEnvironments[device->getRootDeviceIndex()]);
    auto p2 = std::make_unique<MockProgram>(*executionEnvironment);
    p2->setDevice(&device->getDevice());
    retVal = p2->compile(0, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_OUT_OF_HOST_MEMORY, retVal);
    p2.reset(nullptr);
    std::swap(rootDeviceEnvironment, executionEnvironment->rootDeviceEnvironments[device->getRootDeviceIndex()]);

    // fail compilation - any compilation error (here caused by specifying unrecognized option)
    retVal = pProgram->compile(0, nullptr, "-invalid-option", 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_COMPILE_PROGRAM_FAILURE, retVal);

    // compile successfully without notifyFunc
    retVal = pProgram->compile(0, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // compile successfully with notifyFunc
    data[0] = 0;
    retVal = pProgram->compile(0, nullptr, nullptr, 0, nullptr, nullptr, notifyFunc, &data[0]);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ('a', data[0]);
}

TEST_P(ProgramFromSourceTest, GivenFlagsWhenCompilingProgramThenBuildOptionsHaveBeenApplied) {
    auto cip = new MockCompilerInterfaceCaptureBuildOptions();
    auto pDevice = pContext->getDevice(0);
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->compilerInterface.reset(cip);
    auto program = std::make_unique<SucceedingGenBinaryProgram>(*pDevice->getExecutionEnvironment());
    program->setDevice(&pDevice->getDevice());
    program->sourceCode = "__kernel mock() {}";

    // Ask to build created program without NEO::CompilerOptions::gtpinRera and NEO::CompilerOptions::greaterThan4gbBuffersRequired flags.
    cl_int retVal = program->compile(0, nullptr, CompilerOptions::fastRelaxedMath.data(), 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // Check build options that were applied
    EXPECT_TRUE(CompilerOptions::contains(cip->buildOptions, CompilerOptions::fastRelaxedMath)) << cip->buildOptions;
    EXPECT_FALSE(CompilerOptions::contains(cip->buildInternalOptions, CompilerOptions::gtpinRera)) << cip->buildInternalOptions;
    EXPECT_FALSE(CompilerOptions::contains(cip->buildInternalOptions, CompilerOptions::greaterThan4gbBuffersRequired)) << cip->buildInternalOptions;
    EXPECT_TRUE(CompilerOptions::contains(cip->buildInternalOptions, pPlatform->getClDevice(0)->peekCompilerExtensions())) << cip->buildInternalOptions;

    // Ask to build created program with NEO::CompilerOptions::gtpinRera and NEO::CompilerOptions::greaterThan4gbBuffersRequired flags.
    cip->buildOptions.clear();
    cip->buildInternalOptions.clear();
    auto options = CompilerOptions::concatenate(CompilerOptions::greaterThan4gbBuffersRequired, CompilerOptions::gtpinRera, CompilerOptions::finiteMathOnly);
    retVal = program->compile(0, nullptr, options.c_str(),
                              0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // Check build options that were applied
    EXPECT_FALSE(CompilerOptions::contains(cip->buildOptions, CompilerOptions::fastRelaxedMath)) << cip->buildOptions;
    EXPECT_TRUE(CompilerOptions::contains(cip->buildOptions, CompilerOptions::finiteMathOnly)) << cip->buildOptions;
    EXPECT_TRUE(CompilerOptions::contains(cip->buildInternalOptions, CompilerOptions::gtpinRera)) << cip->buildInternalOptions;
    EXPECT_TRUE(CompilerOptions::contains(cip->buildInternalOptions, CompilerOptions::greaterThan4gbBuffersRequired)) << cip->buildInternalOptions;
    EXPECT_TRUE(CompilerOptions::contains(cip->buildInternalOptions, pPlatform->getClDevice(0)->peekCompilerExtensions())) << cip->buildInternalOptions;
}

TEST_F(ProgramTests, GivenFlagsWhenLinkingProgramThenBuildOptionsHaveBeenApplied) {
    auto cip = new MockCompilerInterfaceCaptureBuildOptions();
    auto pProgram = std::make_unique<SucceedingGenBinaryProgram>(*pDevice->getExecutionEnvironment());
    pProgram->setDevice(pDevice);
    pProgram->sourceCode = "__kernel mock() {}";
    pProgram->createdFrom = Program::CreatedFrom::SOURCE;

    cl_program program = pProgram.get();

    // compile successfully a kernel to be linked later
    cl_int retVal = pProgram->compile(0, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // Ask to link created program with NEO::CompilerOptions::gtpinRera and NEO::CompilerOptions::greaterThan4gbBuffersRequired flags.
    auto options = CompilerOptions::concatenate(CompilerOptions::greaterThan4gbBuffersRequired, CompilerOptions::gtpinRera, CompilerOptions::finiteMathOnly);

    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->compilerInterface.reset(cip);

    retVal = pProgram->link(0, nullptr, options.c_str(), 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // Check build options that were applied
    EXPECT_FALSE(CompilerOptions::contains(cip->buildOptions, CompilerOptions::fastRelaxedMath)) << cip->buildOptions;
    EXPECT_TRUE(CompilerOptions::contains(cip->buildOptions, CompilerOptions::finiteMathOnly)) << cip->buildOptions;
    EXPECT_TRUE(CompilerOptions::contains(cip->buildInternalOptions, CompilerOptions::gtpinRera)) << cip->buildInternalOptions;
    EXPECT_TRUE(CompilerOptions::contains(cip->buildInternalOptions, CompilerOptions::greaterThan4gbBuffersRequired)) << cip->buildInternalOptions;
}

TEST_P(ProgramFromSourceTest, GivenAdvancedOptionsWhenCreatingProgramThenSuccessIsReturned) {
    std::string testFile;
    size_t sourceSize = 0;

    Program *p;
    testFile.append(clFiles);
    testFile.append("CopyBuffer_simd16.cl");
    auto pSourceBuffer = loadDataFromFile(testFile.c_str(), sourceSize);
    const char *sources[1] = {pSourceBuffer.get()};
    EXPECT_NE(nullptr, pSourceBuffer);

    //According to spec: If lengths is NULL, all strings in the strings argument are considered null-terminated.
    p = Program::create(pContext, 1, sources, nullptr, retVal);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(nullptr, p);
    delete p;

    //According to spec: If an element in lengths is zero, its accompanying string is null-terminated.
    p = Program::create(pContext, 1, sources, &sourceSize, retVal);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(nullptr, p);
    delete p;

    std::stringstream dataStream(pSourceBuffer.get());
    std::string line;
    std::vector<const char *> lines;
    while (std::getline(dataStream, line, '\n')) {
        char *ptr = new char[line.length() + 1]();
        strcpy_s(ptr, line.length() + 1, line.c_str());
        lines.push_back(ptr);
    }

    // Work on array of strings
    p = Program::create(pContext, 1, &lines[0], nullptr, retVal);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(nullptr, p);
    delete p;

    std::vector<size_t> sizes;
    for (auto ptr : lines)
        sizes.push_back(strlen(ptr));
    sizes[sizes.size() / 2] = 0;

    p = Program::create(pContext, (cl_uint)sizes.size(), &lines[0], &sizes[0], retVal);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(nullptr, p);
    delete p;

    for (auto ptr : lines)
        delete[] ptr;
}

TEST_P(ProgramFromSourceTest, GivenSpecificParamatersWhenLinkingProgramThenSuccessOrCorrectErrorCodeIsReturned) {
    cl_device_id usedDevice = pPlatform->getClDevice(0);
    CreateProgramWithSource(
        pContext,
        &usedDevice,
        SourceFileName);

    cl_device_id deviceList = {0};
    char data[4];
    cl_program program = pProgram;
    cl_program nullprogram = nullptr;
    cl_program invprogram = (cl_program)pContext;

    // Order of following microtests is important - do not change.
    // Add new microtests at end.

    // invalid link parameters: combinations of numDevices & deviceList
    retVal = pProgram->link(1, nullptr, nullptr, 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    retVal = pProgram->link(0, &deviceList, nullptr, 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    // invalid link parameters: combinations of numInputPrograms & inputPrograms
    retVal = pProgram->link(0, nullptr, nullptr, 0, &program, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    retVal = pProgram->link(0, nullptr, nullptr, 1, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    // invalid link parameters: combinations of funcNotify & userData with valid numInputPrograms & inputPrograms
    retVal = pProgram->link(0, nullptr, nullptr, 1, &program, nullptr, &data[0]);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);

    // invalid link parameters: invalid content of deviceList
    retVal = pProgram->link(1, &deviceList, nullptr, 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_DEVICE, retVal);

    // fail linking - another linking is already in progress
    pProgram->setBuildStatus(CL_BUILD_IN_PROGRESS);
    retVal = pProgram->link(0, nullptr, nullptr, 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_OPERATION, retVal);
    pProgram->setBuildStatus(CL_BUILD_NONE);

    // invalid link parameters: invalid Program object==nullptr
    retVal = pProgram->link(0, nullptr, nullptr, 1, &nullprogram, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_PROGRAM, retVal);

    // invalid link parameters: invalid Program object==non Program object
    retVal = pProgram->link(0, nullptr, nullptr, 1, &invprogram, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_PROGRAM, retVal);

    // compile successfully a kernel to be linked later
    retVal = pProgram->compile(0, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // fail linking - code to be linked does not exist
    bool isSpirvTmp = pProgram->getIsSpirV();
    char *pIrBin = pProgram->irBinary.get();
    pProgram->irBinary.release();
    size_t irBinSize = pProgram->irBinarySize;
    pProgram->setIrBinary(nullptr, false);
    retVal = pProgram->link(0, nullptr, nullptr, 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_PROGRAM, retVal);
    pProgram->setIrBinary(pIrBin, isSpirvTmp);

    // fail linking - size of code to be linked is == 0
    pProgram->setIrBinarySize(0, isSpirvTmp);
    retVal = pProgram->link(0, nullptr, nullptr, 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_PROGRAM, retVal);
    pProgram->setIrBinarySize(irBinSize, isSpirvTmp);

    // fail linking - any link error (here caused by specifying unrecognized option)
    retVal = pProgram->link(0, nullptr, "-invalid-option", 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_LINK_PROGRAM_FAILURE, retVal);

    // fail linking - linked code is corrupted and cannot be postprocessed
    auto device = static_cast<ClDevice *>(usedDevice);
    auto p2 = std::make_unique<FailingGenBinaryProgram>(*device->getExecutionEnvironment());
    p2->setDevice(&device->getDevice());
    retVal = p2->link(0, nullptr, nullptr, 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_BINARY, retVal);
    p2.reset(nullptr);

    // link successfully without notifyFunc
    retVal = pProgram->link(0, nullptr, nullptr, 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // link successfully with notifyFunc
    data[0] = 0;
    retVal = pProgram->link(0, nullptr, "", 1, &program, notifyFunc, &data[0]);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ('a', data[0]);
}

TEST_P(ProgramFromSourceTest, GivenInvalidOptionsWhenCreatingLibraryThenCorrectErrorIsReturned) {
    cl_program program = pProgram;

    // Order of following microtests is important - do not change.
    // Add new microtests at end.

    // compile successfully a kernel to be later used to create library
    retVal = pProgram->compile(0, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // create library successfully
    retVal = pProgram->link(0, nullptr, CompilerOptions::createLibrary.data(), 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // fail library creation - any link error (here caused by specifying unrecognized option)
    retVal = pProgram->link(0, nullptr, CompilerOptions::concatenate(CompilerOptions::createLibrary, "-invalid-option").c_str(), 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_LINK_PROGRAM_FAILURE, retVal);

    auto device = pContext->getDevice(0);
    auto executionEnvironment = device->getExecutionEnvironment();
    std::unique_ptr<RootDeviceEnvironment> rootDeviceEnvironment = std::make_unique<NoCompilerInterfaceRootDeviceEnvironment>(*executionEnvironment);
    std::swap(rootDeviceEnvironment, executionEnvironment->rootDeviceEnvironments[device->getRootDeviceIndex()]);
    auto failingProgram = std::make_unique<MockProgram>(*executionEnvironment);
    failingProgram->setDevice(&device->getDevice());

    // fail library creation - CompilerInterface cannot be obtained
    retVal = failingProgram->link(0, nullptr, CompilerOptions::createLibrary.data(), 1, &program, nullptr, nullptr);
    EXPECT_EQ(CL_OUT_OF_HOST_MEMORY, retVal);
    std::swap(rootDeviceEnvironment, executionEnvironment->rootDeviceEnvironments[device->getRootDeviceIndex()]);
}

class PatchTokenFromBinaryTest : public ProgramSimpleFixture {
  public:
    void SetUp() override {
        ProgramSimpleFixture::SetUp();
    }
    void TearDown() override {
        ProgramSimpleFixture::TearDown();
    }
};
using PatchTokenTests = Test<PatchTokenFromBinaryTest>;

template <typename FamilyType>
class CommandStreamReceiverMock : public UltCommandStreamReceiver<FamilyType> {
    using BaseClass = UltCommandStreamReceiver<FamilyType>;
    using BaseClass::BaseClass;

  public:
    void makeResident(GraphicsAllocation &graphicsAllocation) override {
        residency[graphicsAllocation.getUnderlyingBuffer()] = graphicsAllocation.getUnderlyingBufferSize();
        CommandStreamReceiver::makeResident(graphicsAllocation);
    }

    void makeNonResident(GraphicsAllocation &graphicsAllocation) override {
        residency.erase(graphicsAllocation.getUnderlyingBuffer());
        CommandStreamReceiver::makeNonResident(graphicsAllocation);
    }

    std::map<const void *, size_t> residency;
};

HWTEST_F(PatchTokenTests, givenKernelRequiringConstantAllocationWhenMakeResidentIsCalledThenConstantAllocationIsMadeResident) {
    cl_device_id device = pClDevice;

    CreateProgramFromBinary(pContext, &device, "test_constant_memory");

    ASSERT_NE(nullptr, pProgram);
    retVal = pProgram->build(
        1,
        &device,
        nullptr,
        nullptr,
        nullptr,
        false);

    ASSERT_EQ(CL_SUCCESS, retVal);

    auto pKernelInfo = pProgram->getKernelInfo("test");

    EXPECT_NE(nullptr, pKernelInfo->patchInfo.pAllocateStatelessConstantMemorySurfaceWithInitialization);
    ASSERT_NE(nullptr, pProgram->getConstantSurface(pClDevice->getRootDeviceIndex()));

    uint32_t expected_values[] = {0xabcd5432u, 0xaabb5533u};
    uint32_t *constBuff = reinterpret_cast<uint32_t *>(pProgram->getConstantSurface(pClDevice->getRootDeviceIndex())->getUnderlyingBuffer());
    EXPECT_EQ(expected_values[0], constBuff[0]);
    EXPECT_EQ(expected_values[1], constBuff[1]);

    std::unique_ptr<Kernel> pKernel(Kernel::create(pProgram, *pKernelInfo, &retVal));

    ASSERT_EQ(CL_SUCCESS, retVal);
    ASSERT_NE(nullptr, pKernel);

    auto pCommandStreamReceiver = new CommandStreamReceiverMock<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex());
    ASSERT_NE(nullptr, pCommandStreamReceiver);

    pDevice->resetCommandStreamReceiver(pCommandStreamReceiver);
    pCommandStreamReceiver->residency.clear();

    pKernel->makeResident(*pCommandStreamReceiver);
    EXPECT_EQ(2u, pCommandStreamReceiver->residency.size());

    auto &residencyVector = pCommandStreamReceiver->getResidencyAllocations();

    //we expect kernel ISA here and constant allocation
    auto kernelIsa = pKernel->getKernelInfo().getGraphicsAllocation();
    auto constantAllocation = pProgram->getConstantSurface(pDevice->getRootDeviceIndex());

    auto element = std::find(residencyVector.begin(), residencyVector.end(), kernelIsa);
    EXPECT_NE(residencyVector.end(), element);
    element = std::find(residencyVector.begin(), residencyVector.end(), constantAllocation);
    EXPECT_NE(residencyVector.end(), element);

    auto crossThreadData = pKernel->getCrossThreadData();
    uint32_t *constBuffGpuAddr = reinterpret_cast<uint32_t *>(pProgram->getConstantSurface(pContext->getDevice(0)->getRootDeviceIndex())->getGpuAddressToPatch());
    uintptr_t *pDst = reinterpret_cast<uintptr_t *>(crossThreadData + pKernelInfo->patchInfo.pAllocateStatelessConstantMemorySurfaceWithInitialization->DataParamOffset);

    EXPECT_EQ(*pDst, reinterpret_cast<uintptr_t>(constBuffGpuAddr));

    pCommandStreamReceiver->makeSurfacePackNonResident(pCommandStreamReceiver->getResidencyAllocations());
    EXPECT_EQ(0u, pCommandStreamReceiver->residency.size());

    std::vector<Surface *> surfaces;
    pKernel->getResidency(surfaces);
    EXPECT_EQ(2u, surfaces.size());

    for (Surface *surface : surfaces) {
        delete surface;
    }
}

TEST_F(PatchTokenTests, WhenBuildingProgramThenGwsIsSet) {
    cl_device_id device = pClDevice;

    CreateProgramFromBinary(pContext, &device, "kernel_data_param");

    ASSERT_NE(nullptr, pProgram);
    retVal = pProgram->build(
        1,
        &device,
        nullptr,
        nullptr,
        nullptr,
        false);

    ASSERT_EQ(CL_SUCCESS, retVal);

    auto pKernelInfo = pProgram->getKernelInfo("test");

    ASSERT_NE(nullptr, pKernelInfo->patchInfo.dataParameterStream);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.globalWorkSizeOffsets[0]);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.globalWorkSizeOffsets[1]);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.globalWorkSizeOffsets[2]);
}

TEST_F(PatchTokenTests, WhenBuildingProgramThenLwsIsSet) {
    cl_device_id device = pClDevice;

    CreateProgramFromBinary(pContext, &device, "kernel_data_param");

    ASSERT_NE(nullptr, pProgram);
    retVal = pProgram->build(
        1,
        &device,
        nullptr,
        nullptr,
        nullptr,
        false);

    ASSERT_EQ(CL_SUCCESS, retVal);

    auto pKernelInfo = pProgram->getKernelInfo("test");

    ASSERT_NE(nullptr, pKernelInfo->patchInfo.dataParameterStream);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.localWorkSizeOffsets[0]);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.localWorkSizeOffsets[1]);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.localWorkSizeOffsets[2]);

    pKernelInfo = pProgram->getKernelInfo("test_get_local_size");

    ASSERT_NE(nullptr, pKernelInfo->patchInfo.dataParameterStream);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.localWorkSizeOffsets[0]);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.localWorkSizeOffsets[1]);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.localWorkSizeOffsets[2]);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.localWorkSizeOffsets2[0]);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.localWorkSizeOffsets2[1]);
    ASSERT_NE(static_cast<uint32_t>(-1), pKernelInfo->workloadInfo.localWorkSizeOffsets2[2]);
}

TEST_F(PatchTokenTests, WhenBuildingProgramThenConstantKernelArgsAreAvailable) {
    // PATCH_TOKEN_STATELESS_CONSTANT_MEMORY_OBJECT_KERNEL_ARGUMENT
    cl_device_id device = pClDevice;

    CreateProgramFromBinary(pContext, &device, "test_basic_constant");

    ASSERT_NE(nullptr, pProgram);
    retVal = pProgram->build(
        1,
        &device,
        nullptr,
        nullptr,
        nullptr,
        false);

    EXPECT_EQ(CL_SUCCESS, retVal);

    auto pKernelInfo = pProgram->getKernelInfo("constant_kernel");
    ASSERT_NE(nullptr, pKernelInfo);

    auto pKernel = Kernel::create(
        pProgram,
        *pKernelInfo,
        &retVal);

    ASSERT_EQ(CL_SUCCESS, retVal);
    ASSERT_NE(nullptr, pKernel);

    uint32_t numArgs;
    retVal = pKernel->getInfo(CL_KERNEL_NUM_ARGS, sizeof(numArgs), &numArgs, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(3u, numArgs);

    uint32_t sizeOfPtr = sizeof(void *);
    EXPECT_EQ(pKernelInfo->kernelArgInfo[0].kernelArgPatchInfoVector[0].size, sizeOfPtr);
    EXPECT_EQ(pKernelInfo->kernelArgInfo[1].kernelArgPatchInfoVector[0].size, sizeOfPtr);

    delete pKernel;
}

TEST_F(PatchTokenTests, GivenVmeKernelWhenBuildingKernelThenArgAvailable) {
    if (!pDevice->getHardwareInfo().capabilityTable.supportsVme) {
        GTEST_SKIP();
    }
    // PATCH_TOKEN_INLINE_VME_SAMPLER_INFO token indicates a VME kernel.
    cl_device_id device = pClDevice;

    CreateProgramFromBinary(pContext, &device, "vme_kernels");

    ASSERT_NE(nullptr, pProgram);
    retVal = pProgram->build(
        1,
        &device,
        nullptr,
        nullptr,
        nullptr,
        false);

    EXPECT_EQ(CL_SUCCESS, retVal);

    auto pKernelInfo = pProgram->getKernelInfo("device_side_block_motion_estimate_intel");
    ASSERT_NE(nullptr, pKernelInfo);
    EXPECT_EQ(true, pKernelInfo->isVmeWorkload);

    auto pKernel = Kernel::create(
        pProgram,
        *pKernelInfo,
        &retVal);

    ASSERT_NE(nullptr, pKernel);

    delete pKernel;
}

class ProgramPatchTokenFromBinaryTest : public ProgramSimpleFixture {
  public:
    void SetUp() override {
        ProgramSimpleFixture::SetUp();
    }
    void TearDown() override {
        ProgramSimpleFixture::TearDown();
    }
};
typedef Test<ProgramPatchTokenFromBinaryTest> ProgramPatchTokenTests;

TEST(ProgramFromBinaryTests, givenBinaryWithInvalidICBEThenErrorIsReturned) {
    cl_int retVal = CL_INVALID_BINARY;

    SProgramBinaryHeader binHeader;
    memset(&binHeader, 0, sizeof(binHeader));
    binHeader.Magic = iOpenCL::MAGIC_CL;
    binHeader.Version = iOpenCL::CURRENT_ICBE_VERSION - 3;
    binHeader.Device = defaultHwInfo->platform.eRenderCoreFamily;
    binHeader.GPUPointerSizeInBytes = 8;
    binHeader.NumberOfKernels = 0;
    binHeader.SteppingId = 0;
    binHeader.PatchListSize = 0;
    size_t binSize = sizeof(SProgramBinaryHeader);

    {
        const unsigned char *binaries[1] = {reinterpret_cast<const unsigned char *>(&binHeader)};
        const cl_device_id deviceId = 0;
        MockContext context;
        std::unique_ptr<Program> pProgram(Program::create<Program>(&context, 0, &deviceId, &binSize, binaries, nullptr, retVal));
        EXPECT_EQ(nullptr, pProgram.get());
        EXPECT_EQ(CL_INVALID_BINARY, retVal);
    }

    {
        // whatever method we choose CL_INVALID_BINARY is always returned
        ExecutionEnvironment executionEnvironment;
        std::unique_ptr<Program> pProgram(Program::createFromGenBinary(executionEnvironment, nullptr, &binHeader, binSize, false, &retVal, nullptr));
        ASSERT_NE(nullptr, pProgram.get());
        EXPECT_EQ(CL_SUCCESS, retVal);

        retVal = pProgram->processGenBinary();
        EXPECT_EQ(CL_INVALID_BINARY, retVal);
    }
}

TEST(ProgramFromBinaryTests, givenEmptyProgramThenErrorIsReturned) {
    cl_int retVal = CL_INVALID_BINARY;

    SProgramBinaryHeader binHeader;
    memset(&binHeader, 0, sizeof(binHeader));
    binHeader.Magic = iOpenCL::MAGIC_CL;
    binHeader.Version = iOpenCL::CURRENT_ICBE_VERSION;
    binHeader.Device = defaultHwInfo->platform.eRenderCoreFamily;
    binHeader.GPUPointerSizeInBytes = 8;
    binHeader.NumberOfKernels = 0;
    binHeader.SteppingId = 0;
    binHeader.PatchListSize = 0;
    size_t binSize = sizeof(SProgramBinaryHeader);

    ExecutionEnvironment executionEnvironment;
    std::unique_ptr<MockProgram> pProgram(MockProgram::createFromGenBinary<MockProgram>(executionEnvironment, nullptr, &binHeader, binSize, false, &retVal, nullptr));
    ASSERT_NE(nullptr, pProgram.get());
    EXPECT_EQ(CL_SUCCESS, retVal);

    pProgram->unpackedDeviceBinary.reset(nullptr);
    retVal = pProgram->processGenBinary();
    EXPECT_EQ(CL_INVALID_BINARY, retVal);
}

INSTANTIATE_TEST_CASE_P(ProgramFromBinaryTests,
                        ProgramFromBinaryTest,
                        ::testing::Combine(
                            ::testing::ValuesIn(BinaryFileNames),
                            ::testing::ValuesIn(KernelNames)));

INSTANTIATE_TEST_CASE_P(ProgramFromSourceTests,
                        ProgramFromSourceTest,
                        ::testing::Combine(
                            ::testing::ValuesIn(SourceFileNames),
                            ::testing::ValuesIn(BinaryForSourceFileNames),
                            ::testing::ValuesIn(KernelNames)));

using ProgramWithDebugSymbolsTests = Test<ProgramSimpleFixture>;

TEST_F(ProgramWithDebugSymbolsTests, GivenProgramCreatedWithDashGOptionWhenGettingProgramBinariesThenDebugDataIsIncluded) {
    cl_device_id device = pClDevice;

    CreateProgramFromBinary(pContext, &device, "CopyBuffer_simd16", "-g");

    ASSERT_NE(nullptr, pProgram);

    retVal = pProgram->build(
        1,
        &device,
        "-g",
        nullptr,
        nullptr,
        false);
    EXPECT_EQ(CL_SUCCESS, retVal);

    size_t paramValueSize = sizeof(size_t);
    size_t paramValueSizeRet = 0;
    size_t size = 0;

    pProgram->packedDeviceBinary.reset();
    pProgram->packedDeviceBinarySize = 0U;

    retVal = pProgram->packDeviceBinary();

    retVal = pProgram->getInfo(
        CL_PROGRAM_BINARY_SIZES,
        paramValueSize,
        &size,
        nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);

    auto testBinary = std::make_unique<char[]>(size);

    retVal = pProgram->getInfo(
        CL_PROGRAM_BINARIES,
        paramValueSize,
        &testBinary,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);

    ArrayRef<const uint8_t> archive(reinterpret_cast<const uint8_t *>(testBinary.get()), size);
    auto productAbbreviation = hardwarePrefix[pDevice->getHardwareInfo().platform.eProductFamily];

    TargetDevice targetDevice = {};

    targetDevice.coreFamily = pDevice->getHardwareInfo().platform.eRenderCoreFamily;
    targetDevice.stepping = pDevice->getHardwareInfo().platform.usRevId;
    targetDevice.maxPointerSizeInBytes = sizeof(uintptr_t);

    std::string decodeErrors;
    std::string decodeWarnings;
    auto singleDeviceBinary = unpackSingleDeviceBinary(archive, ConstStringRef(productAbbreviation, strlen(productAbbreviation)), targetDevice,
                                                       decodeErrors, decodeWarnings);

    EXPECT_FALSE(singleDeviceBinary.debugData.empty());
}

TEST_F(ProgramTests, WhenProgramIsCreatedThenCorrectOclVersionIsInOptions) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableStatelessToStatefulOptimization.set(false);

    MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);
    if (pClDevice->getEnabledClVersion() == 30) {
        EXPECT_TRUE(CompilerOptions::contains(program.getInternalOptions(), "-ocl-version=300")) << program.getInternalOptions();
    } else if (pClDevice->getEnabledClVersion() == 21) {
        EXPECT_TRUE(CompilerOptions::contains(program.getInternalOptions(), "-ocl-version=210")) << program.getInternalOptions();
    } else {
        EXPECT_TRUE(CompilerOptions::contains(program.getInternalOptions(), "-ocl-version=120")) << program.getInternalOptions();
    }
}

TEST_F(ProgramTests, GivenForcedClVersionWhenProgramIsCreatedThenCorrectOclOptionIsPresent) {
    std::pair<unsigned int, std::string> testedValues[] = {
        {0, "-ocl-version=120"},
        {12, "-ocl-version=120"},
        {21, "-ocl-version=210"},
        {30, "-ocl-version=300"}};

    for (auto &testedValue : testedValues) {
        pClDevice->enabledClVersion = testedValue.first;
        MockProgram program{*pDevice->getExecutionEnvironment(), pContext, false, pDevice};
        EXPECT_TRUE(CompilerOptions::contains(program.getInternalOptions(), testedValue.second));
    }
}

TEST_F(ProgramTests, GivenStatelessToStatefulIsDisabledWhenProgramIsCreatedThenGreaterThan4gbBuffersRequiredOptionIsSet) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableStatelessToStatefulOptimization.set(true);

    MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);
    EXPECT_TRUE(CompilerOptions::contains(program.getInternalOptions(), NEO::CompilerOptions::greaterThan4gbBuffersRequired));
}

TEST_F(ProgramTests, WhenCreatingProgramThenBindlessIsEnabledOnlyIfDebugFlagIsEnabled) {
    using namespace testing;
    DebugManagerStateRestore restorer;

    {
        EXPECT_FALSE(DebugManager.flags.UseBindlessBuffers.get());
        EXPECT_FALSE(DebugManager.flags.UseBindlessImages.get());
        MockProgram programNoBindless(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);
        EXPECT_FALSE(CompilerOptions::contains(programNoBindless.getInternalOptions(), CompilerOptions::bindlessBuffers)) << programNoBindless.getInternalOptions();
        EXPECT_FALSE(CompilerOptions::contains(programNoBindless.getInternalOptions(), CompilerOptions::bindlessImages)) << programNoBindless.getInternalOptions();
    }

    {
        DebugManager.flags.UseBindlessBuffers.set(true);
        MockProgram programNoBindless(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);
        EXPECT_TRUE(CompilerOptions::contains(programNoBindless.getInternalOptions(), CompilerOptions::bindlessBuffers)) << programNoBindless.getInternalOptions();
        EXPECT_FALSE(CompilerOptions::contains(programNoBindless.getInternalOptions(), CompilerOptions::bindlessImages)) << programNoBindless.getInternalOptions();
    }

    {
        DebugManager.flags.UseBindlessBuffers.set(false);
        DebugManager.flags.UseBindlessImages.set(true);
        MockProgram programNoBindless(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);
        EXPECT_FALSE(CompilerOptions::contains(programNoBindless.getInternalOptions(), CompilerOptions::bindlessBuffers)) << programNoBindless.getInternalOptions();
        EXPECT_TRUE(CompilerOptions::contains(programNoBindless.getInternalOptions(), CompilerOptions::bindlessImages)) << programNoBindless.getInternalOptions();
    }

    {
        DebugManager.flags.UseBindlessBuffers.set(true);
        DebugManager.flags.UseBindlessImages.set(true);
        MockProgram programNoBindless(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);
        EXPECT_TRUE(CompilerOptions::contains(programNoBindless.getInternalOptions(), CompilerOptions::bindlessBuffers)) << programNoBindless.getInternalOptions();
        EXPECT_TRUE(CompilerOptions::contains(programNoBindless.getInternalOptions(), CompilerOptions::bindlessImages)) << programNoBindless.getInternalOptions();
    }
}

TEST_F(ProgramTests, givenDeviceThatSupportsSharedSystemMemoryAllocationWhenProgramIsCompiledThenItForcesStatelessCompilation) {
    pClDevice->deviceInfo.sharedSystemMemCapabilities = CL_UNIFIED_SHARED_MEMORY_ACCESS_INTEL | CL_UNIFIED_SHARED_MEMORY_ATOMIC_ACCESS_INTEL | CL_UNIFIED_SHARED_MEMORY_CONCURRENT_ACCESS_INTEL | CL_UNIFIED_SHARED_MEMORY_CONCURRENT_ATOMIC_ACCESS_INTEL;
    pClDevice->sharedDeviceInfo.sharedSystemAllocationsSupport = true;
    MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);
    EXPECT_TRUE(CompilerOptions::contains(program.getInternalOptions().c_str(), CompilerOptions::greaterThan4gbBuffersRequired)) << program.getInternalOptions();
}

TEST_F(ProgramTests, GivenForce32BitAddressessWhenProgramIsCreatedThenGreaterThan4gbBuffersRequiredIsCorrectlySet) {
    cl_int retVal = CL_DEVICE_NOT_FOUND;
    auto defaultSetting = DebugManager.flags.DisableStatelessToStatefulOptimization.get();

    DebugManager.flags.DisableStatelessToStatefulOptimization.set(false);
    if (pDevice) {
        const_cast<DeviceInfo *>(&pDevice->getDeviceInfo())->force32BitAddressess = true;
        MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);
        if (pDevice->areSharedSystemAllocationsAllowed()) {
            EXPECT_TRUE(CompilerOptions::contains(program.getInternalOptions(), CompilerOptions::greaterThan4gbBuffersRequired)) << program.getInternalOptions();
        } else {
            EXPECT_FALSE(CompilerOptions::contains(program.getInternalOptions(), NEO::CompilerOptions::greaterThan4gbBuffersRequired)) << program.getInternalOptions();
        }
    } else {
        EXPECT_NE(CL_DEVICE_NOT_FOUND, retVal);
    }
    DebugManager.flags.DisableStatelessToStatefulOptimization.set(defaultSetting);
}

TEST_F(ProgramTests, Given32bitSupportWhenProgramIsCreatedThenGreaterThan4gbBuffersRequiredIsCorrectlySet) {
    auto defaultSetting = DebugManager.flags.DisableStatelessToStatefulOptimization.get();

    DebugManager.flags.DisableStatelessToStatefulOptimization.set(false);
    std::unique_ptr<MockProgram> program{Program::create<MockProgram>("", pContext, *pClDevice, true, nullptr)};
    if ((false == pDevice->areSharedSystemAllocationsAllowed()) && (false == is32bit)) {
        EXPECT_FALSE(CompilerOptions::contains(program->getInternalOptions(), NEO::CompilerOptions::greaterThan4gbBuffersRequired)) << program->getInternalOptions();
    } else {
        EXPECT_TRUE(CompilerOptions::contains(program->getInternalOptions(), NEO::CompilerOptions::greaterThan4gbBuffersRequired)) << program->getInternalOptions();
    }
    DebugManager.flags.DisableStatelessToStatefulOptimization.set(defaultSetting);
}

TEST_F(ProgramTests, GivenStatelessToStatefulIsDisabledWhenProgramIsCreatedThenGreaterThan4gbBuffersRequiredIsCorrectlySet) {
    auto defaultSetting = DebugManager.flags.DisableStatelessToStatefulOptimization.get();

    DebugManager.flags.DisableStatelessToStatefulOptimization.set(true);
    std::unique_ptr<MockProgram> program{Program::create<MockProgram>("", pContext, *pClDevice, true, nullptr)};
    EXPECT_TRUE(CompilerOptions::contains(program->getInternalOptions(), NEO::CompilerOptions::greaterThan4gbBuffersRequired)) << program->getInternalOptions();
    DebugManager.flags.DisableStatelessToStatefulOptimization.set(defaultSetting);
}

TEST_F(ProgramTests, givenProgramWhenItIsCompiledThenItAlwaysHavePreserveVec3TypeInternalOptionSet) {
    std::unique_ptr<MockProgram> program(Program::create<MockProgram>("", pContext, *pClDevice, true, nullptr));
    EXPECT_TRUE(CompilerOptions::contains(program->getInternalOptions(), CompilerOptions::preserveVec3Type)) << program->getInternalOptions();
}

TEST_F(ProgramTests, Force32BitAddressessWhenProgramIsCreatedThenGreaterThan4gbBuffersRequiredIsCorrectlySet) {
    auto defaultSetting = DebugManager.flags.DisableStatelessToStatefulOptimization.get();

    DebugManager.flags.DisableStatelessToStatefulOptimization.set(false);
    const_cast<DeviceInfo *>(&pDevice->getDeviceInfo())->force32BitAddressess = true;
    std::unique_ptr<MockProgram> program{Program::create<MockProgram>("", pContext, *pClDevice, true, nullptr)};
    if (is32bit) {
        EXPECT_TRUE(CompilerOptions::contains(program->getInternalOptions(), CompilerOptions::greaterThan4gbBuffersRequired)) << program->getInternalOptions();
    } else {
        if (false == pDevice->areSharedSystemAllocationsAllowed()) {
            EXPECT_FALSE(CompilerOptions::contains(program->getInternalOptions(), NEO::CompilerOptions::greaterThan4gbBuffersRequired)) << program->getInternalOptions();
        } else {
            EXPECT_TRUE(CompilerOptions::contains(program->getInternalOptions(), NEO::CompilerOptions::greaterThan4gbBuffersRequired)) << program->getInternalOptions();
        }
    }
    DebugManager.flags.DisableStatelessToStatefulOptimization.set(defaultSetting);
}

TEST_F(ProgramTests, GivenStatelessToStatefulBufferOffsetOptimizationWhenProgramIsCreatedThenBufferOffsetArgIsSet) {
    DebugManagerStateRestore dbgRestorer;
    DebugManager.flags.EnableStatelessToStatefulBufferOffsetOpt.set(1);
    cl_int errorCode = CL_SUCCESS;
    const char programSource[] = "program";
    const char *programPointer = programSource;
    const char **programSources = reinterpret_cast<const char **>(&programPointer);
    size_t length = sizeof(programSource);
    std::unique_ptr<MockProgram> program(Program::create<MockProgram>(pContext, 1u, programSources, &length, errorCode));

    EXPECT_TRUE(CompilerOptions::contains(program->getInternalOptions(), CompilerOptions::hasBufferOffsetArg)) << program->getInternalOptions();
}

TEST_F(ProgramTests, givenStatelessToStatefullOptimizationOffWHenProgramIsCreatedThenOptimizationStringIsNotPresent) {
    DebugManagerStateRestore dbgRestorer;
    DebugManager.flags.EnableStatelessToStatefulBufferOffsetOpt.set(0);
    cl_int errorCode = CL_SUCCESS;
    const char programSource[] = "program";
    const char *programPointer = programSource;
    const char **programSources = reinterpret_cast<const char **>(&programPointer);
    size_t length = sizeof(programSource);
    std::unique_ptr<MockProgram> program(Program::create<MockProgram>(pContext, 1u, programSources, &length, errorCode));
    EXPECT_FALSE(CompilerOptions::contains(program->getInternalOptions(), CompilerOptions::hasBufferOffsetArg)) << program->getInternalOptions();
}

TEST_F(ProgramTests, GivenContextWhenCreateProgramThenIncrementContextRefCount) {
    auto initialApiRefCount = pContext->getReference();
    auto initialInternalRefCount = pContext->getRefInternalCount();

    MockProgram *program = new MockProgram(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    EXPECT_EQ(pContext->getReference(), initialApiRefCount);
    EXPECT_EQ(pContext->getRefInternalCount(), initialInternalRefCount + 1);
    program->release();
    EXPECT_EQ(pContext->getReference(), initialApiRefCount);
    EXPECT_EQ(pContext->getRefInternalCount(), initialInternalRefCount);
}

TEST_F(ProgramTests, GivenContextWhenCreateProgramFromSourceThenIncrementContextRefCount) {
    auto initialApiRefCount = pContext->getReference();
    auto initialInternalRefCount = pContext->getRefInternalCount();

    auto tempProgram = Program::create("", nullptr, *pClDevice, false, nullptr);
    EXPECT_FALSE(tempProgram->getIsBuiltIn());
    auto program = Program::create("", pContext, *pClDevice, false, nullptr);
    EXPECT_FALSE(program->getIsBuiltIn());

    EXPECT_EQ(pContext->getReference(), initialApiRefCount);
    EXPECT_EQ(pContext->getRefInternalCount(), initialInternalRefCount + 1);
    program->release();
    EXPECT_EQ(pContext->getReference(), initialApiRefCount);
    EXPECT_EQ(pContext->getRefInternalCount(), initialInternalRefCount);
    tempProgram->release();
    EXPECT_EQ(pContext->getReference(), initialApiRefCount);
    EXPECT_EQ(pContext->getRefInternalCount(), initialInternalRefCount);
}

TEST_F(ProgramTests, GivenContextWhenCreateBuiltInProgramFromSourceThenDontIncrementContextRefCount) {
    auto initialApiRefCount = pContext->getReference();
    auto initialInternalRefCount = pContext->getRefInternalCount();

    auto tempProgram = Program::create("", nullptr, *pClDevice, true, nullptr);
    EXPECT_TRUE(tempProgram->getIsBuiltIn());
    auto program = Program::create("", pContext, *pClDevice, true, nullptr);
    EXPECT_TRUE(program->getIsBuiltIn());

    EXPECT_EQ(pContext->getReference(), initialApiRefCount);
    EXPECT_EQ(pContext->getRefInternalCount(), initialInternalRefCount);
    program->release();
    EXPECT_EQ(pContext->getReference(), initialApiRefCount);
    EXPECT_EQ(pContext->getRefInternalCount(), initialInternalRefCount);
    tempProgram->release();
    EXPECT_EQ(pContext->getReference(), initialApiRefCount);
    EXPECT_EQ(pContext->getRefInternalCount(), initialInternalRefCount);
}

TEST_F(ProgramTests, WhenBuildingProgramThenPointerToProgramIsReturned) {
    cl_int retVal = CL_DEVICE_NOT_FOUND;
    Program *pProgram = Program::create("", pContext, *pClDevice, false, &retVal);
    EXPECT_NE(nullptr, pProgram);
    EXPECT_EQ(CL_SUCCESS, retVal);
    delete pProgram;

    pProgram = Program::create("", pContext, *pClDevice, false, nullptr);
    EXPECT_NE(nullptr, pProgram);
    delete pProgram;
}

TEST_F(ProgramTests, GivenNullBinaryWhenCreatingProgramFromGenBinaryThenInvalidValueErrorIsReturned) {
    cl_int retVal = CL_SUCCESS;
    Program *pProgram = Program::createFromGenBinary(*pDevice->getExecutionEnvironment(), pContext, nullptr, 0, false, &retVal, pDevice);
    EXPECT_EQ(nullptr, pProgram);
    EXPECT_NE(CL_SUCCESS, retVal);
}

TEST_F(ProgramTests, WhenCreatingProgramFromGenBinaryThenSuccessIsReturned) {
    cl_int retVal = CL_INVALID_BINARY;
    char binary[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, '\0'};
    size_t size = 10;

    Program *pProgram = Program::createFromGenBinary(*pDevice->getExecutionEnvironment(), pContext, binary, size, false, &retVal, pDevice);
    EXPECT_NE(nullptr, pProgram);
    EXPECT_EQ(CL_SUCCESS, retVal);

    EXPECT_EQ((uint32_t)CL_PROGRAM_BINARY_TYPE_EXECUTABLE, (uint32_t)pProgram->getProgramBinaryType());
    EXPECT_FALSE(pProgram->getIsBuiltIn());

    cl_device_id deviceId = pContext->getDevice(0);
    cl_build_status status = 0;
    pProgram->getBuildInfo(deviceId, CL_PROGRAM_BUILD_STATUS,
                           sizeof(cl_build_status), &status, nullptr);
    EXPECT_EQ(CL_BUILD_SUCCESS, status);

    delete pProgram;
}

TEST_F(ProgramTests, GivenBuiltInFlagSetWhenCreatingProgramFromGenBinaryThenBuiltInIsCreated) {
    cl_int retVal = CL_INVALID_BINARY;
    char binary[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, '\0'};
    size_t size = 10;

    Program *pProgram = Program::createFromGenBinary(*pDevice->getExecutionEnvironment(), pContext, binary, size, true, &retVal, pDevice);
    EXPECT_NE(nullptr, pProgram);
    EXPECT_EQ(CL_SUCCESS, retVal);

    EXPECT_TRUE(pProgram->getIsBuiltIn());

    delete pProgram;
}

TEST_F(ProgramTests, GivenRetValNullPointerWhenCreatingProgramFromGenBinaryThenSuccessIsReturned) {
    char binary[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, '\0'};
    size_t size = 10;

    Program *pProgram = Program::createFromGenBinary(*pDevice->getExecutionEnvironment(), pContext, binary, size, false, nullptr, pDevice);
    EXPECT_NE(nullptr, pProgram);
    EXPECT_EQ((uint32_t)CL_PROGRAM_BINARY_TYPE_EXECUTABLE, (uint32_t)pProgram->getProgramBinaryType());

    cl_device_id deviceId = pContext->getDevice(0);
    cl_build_status status = 0;
    pProgram->getBuildInfo(deviceId, CL_PROGRAM_BUILD_STATUS,
                           sizeof(cl_build_status), &status, nullptr);
    EXPECT_EQ(CL_BUILD_SUCCESS, status);

    delete pProgram;
}

TEST_F(ProgramTests, GivenNullContextWhenCreatingProgramFromGenBinaryThenSuccessIsReturned) {
    cl_int retVal = CL_INVALID_BINARY;
    char binary[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, '\0'};
    size_t size = 10;

    Program *pProgram = Program::createFromGenBinary(*pDevice->getExecutionEnvironment(), nullptr, binary, size, false, &retVal, pDevice);
    EXPECT_NE(nullptr, pProgram);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ((uint32_t)CL_PROGRAM_BINARY_TYPE_EXECUTABLE, (uint32_t)pProgram->getProgramBinaryType());

    cl_device_id deviceId = nullptr;
    cl_build_status status = 0;
    pProgram->getBuildInfo(deviceId, CL_PROGRAM_BUILD_STATUS,
                           sizeof(cl_build_status), &status, nullptr);
    EXPECT_EQ(CL_BUILD_SUCCESS, status);

    delete pProgram;
}

TEST_F(ProgramTests, givenProgramFromGenBinaryWhenSLMSizeIsBiggerThenDeviceLimitThenReturnError) {
    PatchTokensTestData::ValidProgramWithKernelUsingSlm patchtokensProgram;
    patchtokensProgram.slmMutable->TotalInlineLocalMemorySize = static_cast<uint32_t>(pDevice->getDeviceInfo().localMemSize * 2);
    patchtokensProgram.recalcTokPtr();
    auto program = std::make_unique<MockProgram>(*pDevice->getExecutionEnvironment(), nullptr, false, pDevice);
    program->unpackedDeviceBinary = makeCopy(patchtokensProgram.storage.data(), patchtokensProgram.storage.size());
    program->unpackedDeviceBinarySize = patchtokensProgram.storage.size();
    auto retVal = program->processGenBinary();

    EXPECT_EQ(CL_OUT_OF_RESOURCES, retVal);
}

TEST_F(ProgramTests, GivenNoCompilerInterfaceRootDeviceEnvironmentWhenRebuildingBinaryThenOutOfHostMemoryErrorIsReturned) {
    auto pDevice = pContext->getDevice(0);
    auto executionEnvironment = pDevice->getExecutionEnvironment();
    std::unique_ptr<RootDeviceEnvironment> rootDeviceEnvironment = std::make_unique<NoCompilerInterfaceRootDeviceEnvironment>(*executionEnvironment);
    rootDeviceEnvironment->setHwInfo(&pDevice->getHardwareInfo());
    std::swap(rootDeviceEnvironment, executionEnvironment->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]);
    auto program = std::make_unique<MockProgram>(*executionEnvironment);
    EXPECT_NE(nullptr, program);
    program->setDevice(&pDevice->getDevice());

    // Load a binary program file
    std::string filePath;
    retrieveBinaryKernelFilename(filePath, "CopyBuffer_simd16_", ".bin");
    size_t binarySize = 0;
    auto pBinary = loadDataFromFile(filePath.c_str(), binarySize);
    EXPECT_NE(0u, binarySize);

    // Create program from loaded binary
    cl_int retVal = program->createProgramFromBinary(pBinary.get(), binarySize);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // Ask to rebuild program from its IR binary - it should fail (no Compiler Interface)
    retVal = program->rebuildProgramFromIr();
    EXPECT_EQ(CL_OUT_OF_HOST_MEMORY, retVal);
    std::swap(rootDeviceEnvironment, executionEnvironment->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]);
}

TEST_F(ProgramTests, GivenGtpinReraFlagWhenBuildingProgramThenCorrectOptionsAreSet) {
    auto cip = new MockCompilerInterfaceCaptureBuildOptions();
    auto pDevice = pContext->getDevice(0);
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->compilerInterface.reset(cip);
    auto program = std::make_unique<SucceedingGenBinaryProgram>(*pDevice->getExecutionEnvironment());
    program->setDevice(&pDevice->getDevice());
    program->sourceCode = "__kernel mock() {}";
    program->createdFrom = Program::CreatedFrom::SOURCE;

    // Ask to build created program without NEO::CompilerOptions::gtpinRera flag.
    cl_int retVal = program->build(0, nullptr, CompilerOptions::fastRelaxedMath.data(), nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // Check build options that were applied
    EXPECT_TRUE(CompilerOptions::contains(cip->buildOptions, CompilerOptions::fastRelaxedMath)) << cip->buildOptions;
    EXPECT_FALSE(CompilerOptions::contains(cip->buildOptions, CompilerOptions::gtpinRera)) << cip->buildInternalOptions;

    // Ask to build created program with NEO::CompilerOptions::gtpinRera flag.
    cip->buildOptions.clear();
    cip->buildInternalOptions.clear();
    retVal = program->build(0, nullptr, CompilerOptions::concatenate(CompilerOptions::gtpinRera, CompilerOptions::finiteMathOnly).c_str(), nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // Check build options that were applied
    EXPECT_FALSE(CompilerOptions::contains(cip->buildOptions, CompilerOptions::fastRelaxedMath)) << cip->buildOptions;
    EXPECT_TRUE(CompilerOptions::contains(cip->buildOptions, CompilerOptions::finiteMathOnly)) << cip->buildOptions;
    EXPECT_TRUE(CompilerOptions::contains(cip->buildInternalOptions, CompilerOptions::gtpinRera)) << cip->buildInternalOptions;
}

TEST_F(ProgramTests, GivenFailingGenBinaryProgramWhenRebuildingBinaryThenInvalidBinaryErrorIsReturned) {

    cl_int retVal;

    auto program = std::make_unique<FailingGenBinaryProgram>(*pDevice->getExecutionEnvironment());
    EXPECT_NE(nullptr, program);
    cl_device_id deviceId = pContext->getDevice(0);
    ClDevice *pDevice = castToObject<ClDevice>(deviceId);
    program->setDevice(&pDevice->getDevice());

    // Load a binary program file
    std::string filePath;
    retrieveBinaryKernelFilename(filePath, "CopyBuffer_simd16_", ".bin");
    size_t binarySize = 0;
    auto pBinary = loadDataFromFile(filePath.c_str(), binarySize);
    EXPECT_NE(0u, binarySize);

    // Create program from loaded binary
    retVal = program->createProgramFromBinary(pBinary.get(), binarySize);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // Ask to rebuild program from its IR binary - it should fail (simulated invalid binary)
    retVal = program->rebuildProgramFromIr();
    EXPECT_EQ(CL_INVALID_BINARY, retVal);
}

TEST_F(ProgramTests, GivenZeroPrivateSizeInBlockWhenAllocateBlockProvateSurfacesCalledThenNoSurfaceIsCreated) {
    MockProgram *program = new MockProgram(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    uint32_t crossThreadOffsetBlock = 0;

    KernelInfo *infoBlock = new KernelInfo;

    SPatchAllocateStatelessPrivateSurface *privateSurfaceBlock = new SPatchAllocateStatelessPrivateSurface;
    privateSurfaceBlock->DataParamOffset = crossThreadOffsetBlock;
    privateSurfaceBlock->DataParamSize = 8;
    privateSurfaceBlock->Size = 8;
    privateSurfaceBlock->SurfaceStateHeapOffset = 0;
    privateSurfaceBlock->Token = 0;
    privateSurfaceBlock->PerThreadPrivateMemorySize = 0;
    infoBlock->patchInfo.pAllocateStatelessPrivateSurface = privateSurfaceBlock;

    program->blockKernelManager->addBlockKernelInfo(infoBlock);

    program->allocateBlockPrivateSurfaces(pDevice->getRootDeviceIndex());

    EXPECT_EQ(nullptr, program->getBlockKernelManager()->getPrivateSurface(0));

    delete privateSurfaceBlock;
    delete program;
}

TEST_F(ProgramTests, GivenNonZeroPrivateSizeInBlockWhenAllocateBlockProvateSurfacesCalledThenSurfaceIsCreated) {
    MockProgram *program = new MockProgram(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    uint32_t crossThreadOffsetBlock = 0;

    KernelInfo *infoBlock = new KernelInfo;

    SPatchAllocateStatelessPrivateSurface *privateSurfaceBlock = new SPatchAllocateStatelessPrivateSurface;
    privateSurfaceBlock->DataParamOffset = crossThreadOffsetBlock;
    privateSurfaceBlock->DataParamSize = 8;
    privateSurfaceBlock->Size = 8;
    privateSurfaceBlock->SurfaceStateHeapOffset = 0;
    privateSurfaceBlock->Token = 0;
    privateSurfaceBlock->PerThreadPrivateMemorySize = 1000;
    infoBlock->patchInfo.pAllocateStatelessPrivateSurface = privateSurfaceBlock;

    program->blockKernelManager->addBlockKernelInfo(infoBlock);

    program->allocateBlockPrivateSurfaces(pDevice->getRootDeviceIndex());

    EXPECT_NE(nullptr, program->getBlockKernelManager()->getPrivateSurface(0));

    delete privateSurfaceBlock;
    delete program;
}

TEST_F(ProgramTests, GivenNonZeroPrivateSizeInBlockWhenAllocateBlockProvateSurfacesCalledThenSecondSurfaceIsNotCreated) {
    MockProgram *program = new MockProgram(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    uint32_t crossThreadOffsetBlock = 0;

    KernelInfo *infoBlock = new KernelInfo;

    SPatchAllocateStatelessPrivateSurface *privateSurfaceBlock = new SPatchAllocateStatelessPrivateSurface;
    privateSurfaceBlock->DataParamOffset = crossThreadOffsetBlock;
    privateSurfaceBlock->DataParamSize = 8;
    privateSurfaceBlock->Size = 8;
    privateSurfaceBlock->SurfaceStateHeapOffset = 0;
    privateSurfaceBlock->Token = 0;
    privateSurfaceBlock->PerThreadPrivateMemorySize = 1000;
    infoBlock->patchInfo.pAllocateStatelessPrivateSurface = privateSurfaceBlock;

    program->blockKernelManager->addBlockKernelInfo(infoBlock);

    program->allocateBlockPrivateSurfaces(pDevice->getRootDeviceIndex());

    GraphicsAllocation *privateSurface = program->getBlockKernelManager()->getPrivateSurface(0);

    EXPECT_NE(nullptr, privateSurface);

    program->allocateBlockPrivateSurfaces(pDevice->getRootDeviceIndex());

    GraphicsAllocation *privateSurface2 = program->getBlockKernelManager()->getPrivateSurface(0);

    EXPECT_EQ(privateSurface, privateSurface2);

    delete privateSurfaceBlock;
    delete program;
}

TEST_F(ProgramTests, givenProgramWithBlockKernelsWhenfreeBlockResourcesisCalledThenFreeGraphhicsAllocationsFromBlockKernelManagerIsCalled) {
    MockProgram *program = new MockProgram(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    uint32_t crossThreadOffsetBlock = 0;

    KernelInfo *infoBlock = new KernelInfo;

    SPatchAllocateStatelessPrivateSurface *privateSurfaceBlock = new SPatchAllocateStatelessPrivateSurface;
    privateSurfaceBlock->DataParamOffset = crossThreadOffsetBlock;
    privateSurfaceBlock->DataParamSize = 8;
    privateSurfaceBlock->Size = 8;
    privateSurfaceBlock->SurfaceStateHeapOffset = 0;
    privateSurfaceBlock->Token = 0;
    privateSurfaceBlock->PerThreadPrivateMemorySize = 1000;
    infoBlock->patchInfo.pAllocateStatelessPrivateSurface = privateSurfaceBlock;

    program->blockKernelManager->addBlockKernelInfo(infoBlock);

    GraphicsAllocation *privateSurface = program->getDevice().getMemoryManager()->allocateGraphicsMemoryWithProperties(MockAllocationProperties{pDevice->getRootDeviceIndex(), MemoryConstants::pageSize});
    EXPECT_NE(nullptr, privateSurface);

    program->getBlockKernelManager()->pushPrivateSurface(privateSurface, 0);

    program->freeBlockResources();

    delete privateSurfaceBlock;
    delete program;
}

class Program32BitTests : public ProgramTests {
  public:
    void SetUp() override {
        DebugManager.flags.Force32bitAddressing.set(true);
        ProgramTests::SetUp();
    }
    void TearDown() override {
        ProgramTests::TearDown();
        DebugManager.flags.Force32bitAddressing.set(false);
    }
};

TEST_F(Program32BitTests, givenDeviceWithForce32BitAddressingOnWhenBuiltinIsCreatedThenNoFlagsArePassedAsInternalOptions) {
    MockProgram program(*pDevice->getExecutionEnvironment());
    auto &internalOptions = program.getInternalOptions();
    EXPECT_THAT(internalOptions, testing::HasSubstr(std::string("")));
}

TEST_F(Program32BitTests, givenDeviceWithForce32BitAddressingOnWhenProgramIsCreatedThen32bitFlagIsPassedAsInternalOption) {
    MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);
    auto &internalOptions = program.getInternalOptions();
    std::string s1 = internalOptions;
    size_t pos = s1.find(NEO::CompilerOptions::arch32bit.data());
    if (is64bit) {
        EXPECT_NE(pos, std::string::npos);
    } else {
        EXPECT_EQ(pos, std::string::npos);
    }
}

TEST_F(ProgramTests, givenNewProgramTheStatelessToStatefulBufferOffsetOtimizationIsMatchingThePlatformEnablingStatus) {
    MockProgram prog(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);
    auto &internalOpts = prog.getInternalOptions();

    HardwareCapabilities hwCaps = {0};
    HwHelper::get(prog.getDevice().getHardwareInfo().platform.eRenderCoreFamily).setupHardwareCapabilities(&hwCaps, prog.getDevice().getHardwareInfo());
    if (hwCaps.isStatelesToStatefullWithOffsetSupported) {
        EXPECT_TRUE(CompilerOptions::contains(internalOpts, CompilerOptions::hasBufferOffsetArg));
    } else {
        EXPECT_FALSE(CompilerOptions::contains(internalOpts, CompilerOptions::hasBufferOffsetArg));
    }
}

template <int32_t ErrCodeToReturn, bool spirv = true>
struct CreateProgramFromBinaryMock : public MockProgram {
    CreateProgramFromBinaryMock(ExecutionEnvironment &executionEnvironment, Context *context, bool isBuiltIn, Device *device)
        : MockProgram(executionEnvironment, context, isBuiltIn, nullptr) {
    }

    cl_int createProgramFromBinary(const void *pBinary,
                                   size_t binarySize) override {
        this->irBinary.reset(new char[binarySize]);
        this->irBinarySize = binarySize;
        this->isSpirV = spirv;
        memcpy_s(this->irBinary.get(), binarySize, pBinary, binarySize);
        return ErrCodeToReturn;
    }
};

TEST_F(ProgramTests, GivenFailedBinaryWhenCreatingFromIlThenInvalidBinaryErrorIsReturned) {
    REQUIRE_OCL_21_OR_SKIP(pContext);
    const uint32_t notSpirv[16] = {0xDEADBEEF};
    cl_int retVal = CL_SUCCESS;
    auto prog = Program::createFromIL<CreateProgramFromBinaryMock<CL_INVALID_BINARY>>(pContext, reinterpret_cast<const void *>(notSpirv), sizeof(notSpirv), retVal);
    EXPECT_EQ(nullptr, prog);
    EXPECT_EQ(CL_INVALID_BINARY, retVal);
}

TEST_F(ProgramTests, GivenSuccessfullyBuiltBinaryWhenCreatingFromIlThenValidProgramIsReturned) {
    REQUIRE_OCL_21_OR_SKIP(pContext);
    const uint32_t spirv[16] = {0x03022307};
    cl_int retVal = CL_SUCCESS;
    auto prog = Program::createFromIL<CreateProgramFromBinaryMock<CL_SUCCESS>>(pContext, reinterpret_cast<const void *>(spirv), sizeof(spirv), retVal);
    ASSERT_NE(nullptr, prog);
    EXPECT_EQ(CL_SUCCESS, retVal);
    prog->release();
}

TEST_F(ProgramTests, givenProgramCreatedFromILWhenCompileIsCalledThenReuseTheILInsteadOfCallingCompilerInterface) {
    REQUIRE_OCL_21_OR_SKIP(pContext);
    const uint32_t spirv[16] = {0x03022307};
    cl_int errCode = 0;
    auto prog = Program::createFromIL<MockProgram>(pContext, reinterpret_cast<const void *>(spirv), sizeof(spirv), errCode);
    ASSERT_NE(nullptr, prog);
    cl_device_id deviceId = pClDevice;
    auto debugVars = NEO::getIgcDebugVars();
    debugVars.forceBuildFailure = true;
    gEnvironment->fclPushDebugVars(debugVars);
    auto compilerErr = prog->compile(1, &deviceId, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, compilerErr);
    gEnvironment->fclPopDebugVars();
    prog->release();
}

TEST_F(ProgramTests, givenProgramCreatedFromIntermediateBinaryRepresentationWhenCompileIsCalledThenReuseTheILInsteadOfCallingCompilerInterface) {
    const uint32_t spirv[16] = {0x03022307};
    cl_int errCode = 0;
    cl_device_id deviceId = pClDevice;
    cl_context ctx = pContext;
    size_t lengths = sizeof(spirv);
    const unsigned char *binaries[1] = {reinterpret_cast<const unsigned char *>(spirv)};
    auto prog = Program::create<MockProgram>(ctx, 1U, &deviceId, &lengths, binaries, nullptr, errCode);
    ASSERT_NE(nullptr, prog);
    auto debugVars = NEO::getIgcDebugVars();
    debugVars.forceBuildFailure = true;
    gEnvironment->fclPushDebugVars(debugVars);
    auto compilerErr = prog->compile(1, &deviceId, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, compilerErr);
    gEnvironment->fclPopDebugVars();
    prog->release();
}

TEST_F(ProgramTests, GivenIlIsNullptrWhenCreatingFromIlThenInvalidBinaryErrorIsReturned) {
    REQUIRE_OCL_21_OR_SKIP(pContext);
    cl_int retVal = CL_SUCCESS;
    auto prog = Program::createFromIL<CreateProgramFromBinaryMock<CL_INVALID_BINARY>>(pContext, nullptr, 16, retVal);
    EXPECT_EQ(nullptr, prog);
    EXPECT_EQ(CL_INVALID_BINARY, retVal);
}

TEST_F(ProgramTests, GivenIlSizeZeroWhenCreatingFromIlThenInvalidBinaryErrorIsReturned) {
    REQUIRE_OCL_21_OR_SKIP(pContext);
    const uint32_t spirv[16] = {0x03022307};
    cl_int retVal = CL_SUCCESS;
    auto prog = Program::createFromIL<CreateProgramFromBinaryMock<CL_INVALID_BINARY>>(pContext, reinterpret_cast<const void *>(spirv), 0, retVal);
    EXPECT_EQ(nullptr, prog);
    EXPECT_EQ(CL_INVALID_BINARY, retVal);
}

TEST_F(ProgramTests, WhenCreatingFromIlThenIsSpirvIsSetCorrectly) {
    REQUIRE_OCL_21_OR_SKIP(pContext);
    const uint32_t spirv[16] = {0x03022307};
    cl_int retVal = CL_SUCCESS;
    auto prog = Program::createFromIL<Program>(pContext, reinterpret_cast<const void *>(spirv), sizeof(spirv), retVal);
    EXPECT_NE(nullptr, prog);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_TRUE(prog->getIsSpirV());
    prog->release();

    const char llvmBc[16] = {'B', 'C', '\xc0', '\xde'};
    prog = Program::createFromIL<Program>(pContext, reinterpret_cast<const void *>(llvmBc), sizeof(llvmBc), retVal);
    EXPECT_NE(nullptr, prog);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_FALSE(prog->getIsSpirV());
    prog->release();
}

static const uint8_t llvmBinary[] = "BC\xc0\xde     ";

TEST(isValidLlvmBinary, whenLlvmMagicWasFoundThenBinaryIsValidLLvm) {
    EXPECT_TRUE(NEO::isLlvmBitcode(llvmBinary));
}

TEST(isValidLlvmBinary, whenBinaryIsNullptrThenBinaryIsNotValidLLvm) {
    EXPECT_FALSE(NEO::isLlvmBitcode(ArrayRef<const uint8_t>()));
}

TEST(isValidLlvmBinary, whenBinaryIsShorterThanLllvMagicThenBinaryIsNotValidLLvm) {
    EXPECT_FALSE(NEO::isLlvmBitcode(ArrayRef<const uint8_t>(llvmBinary, 2)));
}

TEST(isValidLlvmBinary, whenBinaryDoesNotContainLllvMagicThenBinaryIsNotValidLLvm) {
    const uint8_t notLlvmBinary[] = "ABCDEFGHIJKLMNO";
    EXPECT_FALSE(NEO::isLlvmBitcode(notLlvmBinary));
}

const uint32_t spirv[16] = {0x03022307};
const uint32_t spirvInvEndianes[16] = {0x07230203};

TEST(isValidSpirvBinary, whenSpirvMagicWasFoundThenBinaryIsValidSpirv) {
    EXPECT_TRUE(NEO::isSpirVBitcode(ArrayRef<const uint8_t>(reinterpret_cast<const uint8_t *>(&spirv), sizeof(spirv))));
    EXPECT_TRUE(NEO::isSpirVBitcode(ArrayRef<const uint8_t>(reinterpret_cast<const uint8_t *>(&spirvInvEndianes), sizeof(spirvInvEndianes))));
}

TEST(isValidSpirvBinary, whenBinaryIsNullptrThenBinaryIsNotValidLLvm) {
    EXPECT_FALSE(NEO::isSpirVBitcode(ArrayRef<const uint8_t>()));
}

TEST(isValidSpirvBinary, whenBinaryIsShorterThanLllvMagicThenBinaryIsNotValidLLvm) {
    EXPECT_FALSE(NEO::isSpirVBitcode(ArrayRef<const uint8_t>(reinterpret_cast<const uint8_t *>(&spirvInvEndianes), 2)));
}

TEST(isValidSpirvBinary, whenBinaryDoesNotContainLllvMagicThenBinaryIsNotValidLLvm) {
    const uint8_t notSpirvBinary[] = "ABCDEFGHIJKLMNO";
    EXPECT_FALSE(NEO::isSpirVBitcode(notSpirvBinary));
}

TEST_F(ProgramTests, WhenLinkingTwoValidSpirvProgramsThenValidProgramIsReturned) {
    REQUIRE_OCL_21_OR_SKIP(pContext);
    const uint32_t spirv[16] = {0x03022307};
    cl_int errCode = CL_SUCCESS;

    auto node1 = Program::createFromIL<CreateProgramFromBinaryMock<CL_SUCCESS, false>>(pContext, reinterpret_cast<const void *>(spirv), sizeof(spirv), errCode);
    ASSERT_NE(nullptr, node1);
    EXPECT_EQ(CL_SUCCESS, errCode);

    auto node2 = Program::createFromIL<CreateProgramFromBinaryMock<CL_SUCCESS>>(pContext, reinterpret_cast<const void *>(spirv), sizeof(spirv), errCode);
    ASSERT_NE(nullptr, node2);
    EXPECT_EQ(CL_SUCCESS, errCode);

    auto prog = Program::createFromIL<CreateProgramFromBinaryMock<CL_SUCCESS>>(pContext, reinterpret_cast<const void *>(spirv), sizeof(spirv), errCode);
    ASSERT_NE(nullptr, prog);
    EXPECT_EQ(CL_SUCCESS, errCode);

    cl_program linkNodes[] = {node1, node2};
    errCode = prog->link(0, nullptr, nullptr, 2, linkNodes, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, errCode);

    prog->release();
    node2->release();
    node1->release();
}

TEST_F(ProgramTests, givenSeparateBlockKernelsWhenNoParentAndSubgroupKernelsThenSeparateNoneKernel) {
    MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    EXPECT_EQ(0u, program.getKernelInfoArray().size());
    EXPECT_EQ(0u, program.getParentKernelInfoArray().size());
    EXPECT_EQ(0u, program.getSubgroupKernelInfoArray().size());

    program.separateBlockKernels();

    EXPECT_EQ(0u, program.getKernelInfoArray().size());
    EXPECT_EQ(0u, program.getBlockKernelManager()->getCount());
}

TEST_F(ProgramTests, givenSeparateBlockKernelsWhenRegularKernelsThenSeparateNoneKernel) {
    MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    auto pRegularKernel1Info = new KernelInfo();
    pRegularKernel1Info->name = "regular_kernel_1";
    program.getKernelInfoArray().push_back(pRegularKernel1Info);

    auto pRegularKernel2Info = new KernelInfo();
    pRegularKernel2Info->name = "regular_kernel_2";
    program.getKernelInfoArray().push_back(pRegularKernel2Info);

    EXPECT_EQ(2u, program.getKernelInfoArray().size());

    program.separateBlockKernels();

    EXPECT_EQ(2u, program.getKernelInfoArray().size());
    EXPECT_EQ(0, strcmp("regular_kernel_1", program.getKernelInfoArray().at(0)->name.c_str()));
    EXPECT_EQ(0, strcmp("regular_kernel_2", program.getKernelInfoArray().at(1)->name.c_str()));

    EXPECT_EQ(0u, program.getBlockKernelManager()->getCount());
}

TEST_F(ProgramTests, givenSeparateBlockKernelsWhenChildLikeKernelWithoutParentKernelThenSeparateNoneKernel) {
    MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    auto pParentKernelInfo = new KernelInfo();
    pParentKernelInfo->name = "another_parent_kernel";
    program.getKernelInfoArray().push_back(pParentKernelInfo);
    program.getParentKernelInfoArray().push_back(pParentKernelInfo);

    auto pChildKernelInfo = new KernelInfo();
    pChildKernelInfo->name = "childlike_kernel_dispatch_0";
    program.getKernelInfoArray().push_back(pChildKernelInfo);

    EXPECT_EQ(2u, program.getKernelInfoArray().size());
    EXPECT_EQ(1u, program.getParentKernelInfoArray().size());

    program.separateBlockKernels();

    EXPECT_EQ(2u, program.getKernelInfoArray().size());
    EXPECT_EQ(0, strcmp("another_parent_kernel", program.getKernelInfoArray().at(0)->name.c_str()));
    EXPECT_EQ(0, strcmp("childlike_kernel_dispatch_0", program.getKernelInfoArray().at(1)->name.c_str()));

    EXPECT_EQ(0u, program.getBlockKernelManager()->getCount());
}

TEST_F(ProgramTests, givenSeparateBlockKernelsWhenChildLikeKernelWithoutSubgroupKernelThenSeparateNoneKernel) {
    MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    auto pSubgroupKernelInfo = new KernelInfo();
    pSubgroupKernelInfo->name = "another_subgroup_kernel";
    program.getKernelInfoArray().push_back(pSubgroupKernelInfo);
    program.getSubgroupKernelInfoArray().push_back(pSubgroupKernelInfo);

    auto pChildKernelInfo = new KernelInfo();
    pChildKernelInfo->name = "childlike_kernel_dispatch_0";
    program.getKernelInfoArray().push_back(pChildKernelInfo);

    EXPECT_EQ(2u, program.getKernelInfoArray().size());
    EXPECT_EQ(1u, program.getSubgroupKernelInfoArray().size());

    program.separateBlockKernels();

    EXPECT_EQ(2u, program.getKernelInfoArray().size());
    EXPECT_EQ(0, strcmp("another_subgroup_kernel", program.getKernelInfoArray().at(0)->name.c_str()));
    EXPECT_EQ(0, strcmp("childlike_kernel_dispatch_0", program.getKernelInfoArray().at(1)->name.c_str()));

    EXPECT_EQ(0u, program.getBlockKernelManager()->getCount());
}

TEST_F(ProgramTests, givenSeparateBlockKernelsWhenParentKernelWithChildKernelThenSeparateChildKernel) {
    MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    auto pParentKernelInfo = new KernelInfo();
    pParentKernelInfo->name = "parent_kernel";
    program.getKernelInfoArray().push_back(pParentKernelInfo);
    program.getParentKernelInfoArray().push_back(pParentKernelInfo);

    auto pChildKernelInfo = new KernelInfo();
    pChildKernelInfo->name = "parent_kernel_dispatch_0";
    program.getKernelInfoArray().push_back(pChildKernelInfo);

    EXPECT_EQ(2u, program.getKernelInfoArray().size());
    EXPECT_EQ(1u, program.getParentKernelInfoArray().size());

    program.separateBlockKernels();

    EXPECT_EQ(1u, program.getKernelInfoArray().size());
    EXPECT_EQ(0, strcmp("parent_kernel", program.getKernelInfoArray().at(0)->name.c_str()));

    EXPECT_EQ(1u, program.getBlockKernelManager()->getCount());
    EXPECT_EQ(0, strcmp("parent_kernel_dispatch_0", program.getBlockKernelManager()->getBlockKernelInfo(0)->name.c_str()));
}

TEST_F(ProgramTests, givenSeparateBlockKernelsWhenSubgroupKernelWithChildKernelThenSeparateChildKernel) {
    MockProgram program(*pDevice->getExecutionEnvironment(), pContext, false, pDevice);

    auto pSubgroupKernelInfo = new KernelInfo();
    pSubgroupKernelInfo->name = "subgroup_kernel";
    program.getKernelInfoArray().push_back(pSubgroupKernelInfo);
    program.getSubgroupKernelInfoArray().push_back(pSubgroupKernelInfo);

    auto pChildKernelInfo = new KernelInfo();
    pChildKernelInfo->name = "subgroup_kernel_dispatch_0";
    program.getKernelInfoArray().push_back(pChildKernelInfo);

    EXPECT_EQ(2u, program.getKernelInfoArray().size());
    EXPECT_EQ(1u, program.getSubgroupKernelInfoArray().size());

    program.separateBlockKernels();

    EXPECT_EQ(1u, program.getKernelInfoArray().size());
    EXPECT_EQ(0, strcmp("subgroup_kernel", program.getKernelInfoArray().at(0)->name.c_str()));

    EXPECT_EQ(1u, program.getBlockKernelManager()->getCount());
    EXPECT_EQ(0, strcmp("subgroup_kernel_dispatch_0", program.getBlockKernelManager()->getBlockKernelInfo(0)->name.c_str()));
}

TEST(SimpleProgramTests, givenDefaultProgramWhenSetDeviceIsCalledThenDeviceIsSet) {
    ExecutionEnvironment executionEnvironment;
    MockProgram program(executionEnvironment);
    EXPECT_EQ(nullptr, program.getDevicePtr());
    auto dummyDevice = (Device *)0x1337;
    program.setDevice(dummyDevice);
    EXPECT_EQ(dummyDevice, program.getDevicePtr());
    program.setDevice(nullptr);
    EXPECT_EQ(nullptr, program.getDevicePtr());
}

TEST(ProgramDestructionTests, givenProgramUsingDeviceWhenItIsDestroyedAfterPlatfromCleanupThenItIsCleanedUpProperly) {
    initPlatform();
    auto device = platform()->getClDevice(0);
    MockContext *context = new MockContext(device, false);
    MockProgram *pProgram = new MockProgram(*device->getExecutionEnvironment(), context, false, &device->getDevice());
    auto globalAllocation = device->getMemoryManager()->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});
    pProgram->setGlobalSurface(globalAllocation);

    platformsImpl->clear();
    EXPECT_EQ(1, device->getRefInternalCount());
    EXPECT_EQ(1, pProgram->getRefInternalCount());
    context->decRefInternal();
    pProgram->decRefInternal();
}

TEST_F(ProgramTests, givenProgramWithSpirvWhenRebuildProgramIsCalledThenSpirvPathIsTaken) {
    auto device = castToObject<ClDevice>(pContext->getDevice(0));

    auto compilerInterface = new MockCompilerInterface();
    auto compilerMain = new MockCIFMain();
    compilerInterface->setFclMain(compilerMain);
    compilerMain->Retain();
    compilerInterface->setIgcMain(compilerMain);
    compilerMain->setDefaultCreatorFunc<NEO::MockIgcOclDeviceCtx>(NEO::MockIgcOclDeviceCtx::Create);
    compilerMain->setDefaultCreatorFunc<NEO::MockFclOclDeviceCtx>(NEO::MockFclOclDeviceCtx::Create);
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->compilerInterface.reset(compilerInterface);

    std::string receivedInput;
    MockCompilerDebugVars debugVars = {};
    debugVars.receivedInput = &receivedInput;
    debugVars.forceBuildFailure = true;
    gEnvironment->igcPushDebugVars(debugVars);
    std::unique_ptr<void, void (*)(void *)> igcDebugVarsAutoPop{&gEnvironment, [](void *) { gEnvironment->igcPopDebugVars(); }};

    auto program = clUniquePtr(new MockProgram(*pDevice->getExecutionEnvironment()));
    program->setDevice(&device->getDevice());
    uint32_t spirv[16] = {0x03022307, 0x23471113, 0x17192329};
    program->irBinary = makeCopy(spirv, sizeof(spirv));
    program->irBinarySize = sizeof(spirv);
    program->isSpirV = true;
    auto buildRet = program->rebuildProgramFromIr();
    EXPECT_NE(CL_SUCCESS, buildRet);
    ASSERT_EQ(sizeof(spirv), receivedInput.size());
    EXPECT_EQ(0, memcmp(spirv, receivedInput.c_str(), receivedInput.size()));
    ASSERT_EQ(1U, compilerInterface->requestedTranslationCtxs.size());
    EXPECT_EQ(IGC::CodeType::spirV, compilerInterface->requestedTranslationCtxs[0].first);
    EXPECT_EQ(IGC::CodeType::oclGenBin, compilerInterface->requestedTranslationCtxs[0].second);
}

TEST_F(ProgramTests, whenRebuildingProgramThenStoreDeviceBinaryProperly) {
    auto device = castToObject<ClDevice>(pContext->getDevice(0));

    auto compilerInterface = new MockCompilerInterface();
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->compilerInterface.reset(compilerInterface);
    auto compilerMain = new MockCIFMain();
    compilerInterface->setIgcMain(compilerMain);
    compilerMain->setDefaultCreatorFunc<NEO::MockIgcOclDeviceCtx>(NEO::MockIgcOclDeviceCtx::Create);

    MockCompilerDebugVars debugVars = {};
    char binaryToReturn[] = "abcdfghijklmnop";
    debugVars.binaryToReturn = binaryToReturn;
    debugVars.binaryToReturnSize = sizeof(binaryToReturn);
    gEnvironment->igcPushDebugVars(debugVars);
    std::unique_ptr<void, void (*)(void *)> igcDebugVarsAutoPop{&gEnvironment, [](void *) { gEnvironment->igcPopDebugVars(); }};

    auto program = clUniquePtr(new MockProgram(*pDevice->getExecutionEnvironment()));
    program->setDevice(&device->getDevice());
    uint32_t ir[16] = {0x03022307, 0x23471113, 0x17192329};
    program->irBinary = makeCopy(ir, sizeof(ir));
    program->irBinarySize = sizeof(ir);
    EXPECT_EQ(nullptr, program->unpackedDeviceBinary);
    EXPECT_EQ(0U, program->unpackedDeviceBinarySize);
    program->rebuildProgramFromIr();
    ASSERT_NE(nullptr, program->unpackedDeviceBinary);
    ASSERT_EQ(sizeof(binaryToReturn), program->unpackedDeviceBinarySize);
    EXPECT_EQ(0, memcmp(binaryToReturn, program->unpackedDeviceBinary.get(), program->unpackedDeviceBinarySize));
}

TEST_F(ProgramTests, givenProgramWhenInternalOptionsArePassedThenTheyAreAddedToProgramInternalOptions) {
    ExecutionEnvironment executionEnvironment;
    MockProgram program(executionEnvironment);
    program.getInternalOptions().erase();
    EXPECT_EQ(nullptr, program.getDevicePtr());
    std::string buildOptions = NEO::CompilerOptions::gtpinRera.str();
    program.extractInternalOptions(buildOptions);
    EXPECT_STREQ(program.getInternalOptions().c_str(), NEO::CompilerOptions::gtpinRera.data());
}

TEST_F(ProgramTests, givenProgramWhenUnknownInternalOptionsArePassedThenTheyAreNotAddedToProgramInternalOptions) {
    ExecutionEnvironment executionEnvironment;
    MockProgram program(executionEnvironment);
    program.getInternalOptions().erase();
    EXPECT_EQ(nullptr, program.getDevicePtr());
    const char *internalOption = "-unknown-internal-options-123";
    std::string buildOptions(internalOption);
    program.extractInternalOptions(buildOptions);
    EXPECT_EQ(0u, program.getInternalOptions().length());
}

TEST_F(ProgramTests, givenProgramWhenAllInternalOptionsArePassedMixedWithUnknownInputThenTheyAreParsedCorrectly) {
    ExecutionEnvironment executionEnvironment;
    MockProgram program(executionEnvironment);
    program.getInternalOptions().erase();
    EXPECT_EQ(nullptr, program.getDevicePtr());
    std::string buildOptions = CompilerOptions::concatenate("###", CompilerOptions::gtpinRera, "###", CompilerOptions::greaterThan4gbBuffersRequired, "###");
    std::string expectedOutput = CompilerOptions::concatenate(CompilerOptions::gtpinRera, CompilerOptions::greaterThan4gbBuffersRequired);
    program.extractInternalOptions(buildOptions);
    EXPECT_EQ(expectedOutput, program.getInternalOptions());
}

TEST_F(ProgramTests, givenProgramWhenInternalOptionsArePassedWithValidValuesThenTheyAreAddedToProgramInternalOptions) {
    ExecutionEnvironment executionEnvironment;
    MockProgram program(executionEnvironment);
    program.getInternalOptions().erase();
    EXPECT_EQ(nullptr, program.getDevicePtr());

    program.isFlagOptionOverride = false;
    program.isOptionValueValidOverride = true;
    std::string buildOptions = CompilerOptions::concatenate(CompilerOptions::gtpinRera, "someValue");
    program.extractInternalOptions(buildOptions);
    EXPECT_EQ(buildOptions, program.getInternalOptions()) << program.getInternalOptions();
}

TEST_F(ProgramTests, givenProgramWhenInternalOptionsArePassedWithInvalidValuesThenTheyAreNotAddedToProgramInternalOptions) {
    ExecutionEnvironment executionEnvironment;
    MockProgram program(executionEnvironment);
    EXPECT_EQ(nullptr, program.getDevicePtr());

    program.isFlagOptionOverride = false;
    std::string buildOptions = CompilerOptions::concatenate(CompilerOptions::gtpinRera, "someValue");
    std::string expectedOutput = "";
    program.getInternalOptions().erase();
    program.extractInternalOptions(buildOptions);
    EXPECT_EQ(expectedOutput, program.getInternalOptions());

    program.isOptionValueValidOverride = true;
    buildOptions = std::string(CompilerOptions::gtpinRera);
    program.getInternalOptions().erase();
    program.extractInternalOptions(buildOptions);
    EXPECT_EQ(expectedOutput, program.getInternalOptions());
}

class AdditionalOptionsMockProgram : public MockProgram {
  public:
    AdditionalOptionsMockProgram() : MockProgram(executionEnvironment) {}
    void applyAdditionalOptions() override {
        applyAdditionalOptionsCalled++;
        MockProgram::applyAdditionalOptions();
    }
    uint32_t applyAdditionalOptionsCalled = 0;
    ExecutionEnvironment executionEnvironment;
};

TEST_F(ProgramTests, givenProgramWhenBuiltThenAdditionalOptionsAreApplied) {
    AdditionalOptionsMockProgram program;
    program.setDevice(pDevice);
    cl_device_id device = pClDevice;

    program.build(1, &device, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(1u, program.applyAdditionalOptionsCalled);
}

TEST_F(ProgramTests, WhenProgramIsCreatedThenItsDeviceIsProperlySet) {
    auto wasValidClDeviceUsed = [](MockProgram &program) -> bool {
        return (program.getInternalOptions().find(CompilerOptions::arch32bit.data()) != std::string::npos);
    };

    MockExecutionEnvironment executionEnvironment;
    MockDevice mockDevice;
    mockDevice.deviceInfo.force32BitAddressess = true;
    auto pContextMockDevice = new MockDevice;
    MockClDevice contextMockClDevice{pContextMockDevice};
    MockContext mockContext{&contextMockClDevice};

    MockProgram programWithDeviceGiven{executionEnvironment, &mockContext, false, &mockDevice};
    EXPECT_EQ(&mockDevice, programWithDeviceGiven.pDevice);

    MockProgram programWithDeviceFromContext{executionEnvironment, &mockContext, false, nullptr};
    EXPECT_EQ(pContextMockDevice, programWithDeviceFromContext.pDevice);

    MockProgram programWithDeviceWithoutSpecializedDevice{executionEnvironment, nullptr, false, &mockDevice};
    EXPECT_FALSE(wasValidClDeviceUsed(programWithDeviceWithoutSpecializedDevice));

    MockDevice invalidClDevice;
    mockDevice.setSpecializedDevice(&invalidClDevice);
    MockProgram programWithDeviceWithInvalidSpecializedDevice{executionEnvironment, nullptr, false, &mockDevice};
    EXPECT_FALSE(wasValidClDeviceUsed(programWithDeviceWithInvalidSpecializedDevice));

    MockClDevice validClDevice{new MockDevice};
    validClDevice.sharedDeviceInfo.force32BitAddressess = true;
    MockProgram programWithDeviceWithValidSpecializedDevice{executionEnvironment, nullptr, false, &validClDevice.getDevice()};
    EXPECT_TRUE(wasValidClDeviceUsed(programWithDeviceWithValidSpecializedDevice));
}

TEST(CreateProgramFromBinaryTests, givenBinaryProgramWhenKernelRebulildIsForcedThenDeviceBinaryIsNotUsed) {
    DebugManagerStateRestore dbgRestorer;
    DebugManager.flags.RebuildPrecompiledKernels.set(true);
    cl_int retVal = CL_INVALID_BINARY;

    PatchTokensTestData::ValidEmptyProgram programTokens;

    auto clDevice = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));
    std::unique_ptr<MockProgram> pProgram(Program::createFromGenBinary<MockProgram>(*clDevice->getExecutionEnvironment(), nullptr, programTokens.storage.data(), programTokens.storage.size(), false, &retVal, &clDevice->getDevice()));
    pProgram->pDevice = &clDevice->getDevice();
    ASSERT_NE(nullptr, pProgram.get());
    EXPECT_EQ(CL_SUCCESS, retVal);

    retVal = pProgram->createProgramFromBinary(programTokens.storage.data(), programTokens.storage.size());
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(nullptr, pProgram->unpackedDeviceBinary.get());
    EXPECT_EQ(0U, pProgram->unpackedDeviceBinarySize);
    EXPECT_EQ(nullptr, pProgram->packedDeviceBinary);
    EXPECT_EQ(0U, pProgram->packedDeviceBinarySize);
}

TEST(CreateProgramFromBinaryTests, givenBinaryProgramWhenKernelRebulildIsNotForcedThenDeviceBinaryIsUsed) {
    cl_int retVal = CL_INVALID_BINARY;

    PatchTokensTestData::ValidEmptyProgram programTokens;

    auto clDevice = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));
    std::unique_ptr<MockProgram> pProgram(Program::createFromGenBinary<MockProgram>(*clDevice->getExecutionEnvironment(), nullptr, programTokens.storage.data(), programTokens.storage.size(), false, &retVal, &clDevice->getDevice()));
    pProgram->pDevice = &clDevice->getDevice();
    ASSERT_NE(nullptr, pProgram.get());
    EXPECT_EQ(CL_SUCCESS, retVal);

    retVal = pProgram->createProgramFromBinary(programTokens.storage.data(), programTokens.storage.size());
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(nullptr, reinterpret_cast<uint8_t *>(pProgram->unpackedDeviceBinary.get()));
    EXPECT_EQ(programTokens.storage.size(), pProgram->unpackedDeviceBinarySize);
    EXPECT_NE(nullptr, reinterpret_cast<uint8_t *>(pProgram->packedDeviceBinary.get()));
    EXPECT_EQ(programTokens.storage.size(), pProgram->packedDeviceBinarySize);
}

struct SpecializationConstantProgramMock : public MockProgram {
    using MockProgram::MockProgram;
    cl_int updateSpecializationConstant(cl_uint specId, size_t specSize, const void *specValue) override {
        return CL_SUCCESS;
    }
};

struct SpecializationConstantCompilerInterfaceMock : public CompilerInterface {
    TranslationOutput::ErrorCode retVal = TranslationOutput::ErrorCode::Success;
    int counter = 0;
    const char *spirV = nullptr;
    TranslationOutput::ErrorCode getSpecConstantsInfo(const NEO::Device &device, ArrayRef<const char> srcSpirV, SpecConstantInfo &output) override {
        counter++;
        spirV = srcSpirV.begin();
        return retVal;
    }
    void returnError() {
        retVal = TranslationOutput::ErrorCode::CompilationFailure;
    }
};

struct SpecializationConstantRootDeviceEnvironemnt : public RootDeviceEnvironment {
    SpecializationConstantRootDeviceEnvironemnt(ExecutionEnvironment &executionEnvironment) : RootDeviceEnvironment(executionEnvironment) {
        compilerInterface.reset(new SpecializationConstantCompilerInterfaceMock());
    }
    CompilerInterface *getCompilerInterface() override {
        return compilerInterface.get();
    }
};

struct setProgramSpecializationConstantTests : public ::testing::Test {
    void SetUp() override {
        mockCompiler = new SpecializationConstantCompilerInterfaceMock();
        auto rootDeviceEnvironment = device.getExecutionEnvironment()->rootDeviceEnvironments[0].get();
        rootDeviceEnvironment->compilerInterface.reset(mockCompiler);
        mockProgram.reset(new SpecializationConstantProgramMock(*device.getExecutionEnvironment()));
        mockProgram->isSpirV = true;
        mockProgram->setDevice(&device);

        EXPECT_FALSE(mockProgram->areSpecializationConstantsInitialized);
        EXPECT_EQ(0, mockCompiler->counter);
    }

    SpecializationConstantCompilerInterfaceMock *mockCompiler = nullptr;
    std::unique_ptr<SpecializationConstantProgramMock> mockProgram;
    MockDevice device;

    int specValue = 1;
};

TEST_F(setProgramSpecializationConstantTests, whenSetProgramSpecializationConstantThenBinarySourceIsUsed) {
    auto retVal = mockProgram->setProgramSpecializationConstant(1, sizeof(int), &specValue);

    EXPECT_EQ(1, mockCompiler->counter);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_TRUE(mockProgram->areSpecializationConstantsInitialized);
    EXPECT_EQ(mockProgram->irBinary.get(), mockCompiler->spirV);
}

TEST_F(setProgramSpecializationConstantTests, whenSetProgramSpecializationConstantMultipleTimesThenSpecializationConstantsAreInitializedOnce) {
    auto retVal = mockProgram->setProgramSpecializationConstant(1, sizeof(int), &specValue);

    EXPECT_EQ(1, mockCompiler->counter);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_TRUE(mockProgram->areSpecializationConstantsInitialized);

    retVal = mockProgram->setProgramSpecializationConstant(1, sizeof(int), &specValue);

    EXPECT_EQ(1, mockCompiler->counter);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_TRUE(mockProgram->areSpecializationConstantsInitialized);
}

TEST_F(setProgramSpecializationConstantTests, givenInvalidGetSpecConstantsInfoReturnValueWhenSetProgramSpecializationConstantThenErrorIsReturned) {
    mockCompiler->returnError();

    auto retVal = mockProgram->setProgramSpecializationConstant(1, sizeof(int), &specValue);

    EXPECT_EQ(1, mockCompiler->counter);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    EXPECT_FALSE(mockProgram->areSpecializationConstantsInitialized);
}

TEST(setProgramSpecializationConstantTest, givenUninitializedCompilerinterfaceWhenSetProgramSpecializationConstantThenErrorIsReturned) {
    auto executionEnvironment = new MockExecutionEnvironment();
    executionEnvironment->rootDeviceEnvironments[0] = std::make_unique<NoCompilerInterfaceRootDeviceEnvironment>(*executionEnvironment);
    executionEnvironment->rootDeviceEnvironments[0]->setHwInfo(defaultHwInfo.get());
    MockDevice mockDevice(executionEnvironment, 0);
    SpecializationConstantProgramMock mockProgram(*executionEnvironment);
    mockProgram.setDevice(&mockDevice);

    mockProgram.isSpirV = true;
    int specValue = 1;

    auto retVal = mockProgram.setProgramSpecializationConstant(1, sizeof(int), &specValue);
    EXPECT_EQ(CL_OUT_OF_HOST_MEMORY, retVal);
}

using ProgramBinTest = Test<ProgramSimpleFixture>;

TEST_F(ProgramBinTest, givenPrintProgramBinaryProcessingTimeSetWhenBuildProgramThenProcessingTimeIsPrinted) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.PrintProgramBinaryProcessingTime.set(true);
    testing::internal::CaptureStdout();

    cl_device_id device = pClDevice;
    CreateProgramFromBinary(pContext, &device, "kernel_data_param");

    auto retVal = pProgram->build(
        1,
        &device,
        nullptr,
        nullptr,
        nullptr,
        false);

    auto output = testing::internal::GetCapturedStdout();
    EXPECT_FALSE(output.compare(0, 14, "Elapsed time: "));
    EXPECT_EQ(CL_SUCCESS, retVal);
}

struct DebugDataGuard {
    DebugDataGuard(const DebugDataGuard &) = delete;
    DebugDataGuard(DebugDataGuard &&) = delete;

    DebugDataGuard() {
        for (size_t n = 0; n < sizeof(mockDebugData); n++) {
            mockDebugData[n] = (char)n;
        }

        auto vars = NEO::getIgcDebugVars();
        vars.debugDataToReturn = mockDebugData;
        vars.debugDataToReturnSize = sizeof(mockDebugData);
        NEO::setIgcDebugVars(vars);
    }

    ~DebugDataGuard() {
        auto vars = NEO::getIgcDebugVars();
        vars.debugDataToReturn = nullptr;
        vars.debugDataToReturnSize = 0;
        NEO::setIgcDebugVars(vars);
    }

    char mockDebugData[32];
};

TEST_F(ProgramBinTest, GivenBuildWithDebugDataThenBuildDataAvailableViaGetInfo) {
    DebugDataGuard debugDataGuard;

    cl_device_id device = pClDevice;
    const char *sourceCode = "__kernel void\nCB(\n__global unsigned int* src, __global unsigned int* dst)\n{\nint id = (int)get_global_id(0);\ndst[id] = src[id];\n}\n";
    pProgram = Program::create<MockProgram>(
        pContext,
        1,
        &sourceCode,
        &knownSourceSize,
        retVal);
    retVal = pProgram->build(1, &device, nullptr, nullptr, nullptr, false);
    EXPECT_EQ(CL_SUCCESS, retVal);

    // Verify
    size_t debugDataSize = 0;
    retVal = pProgram->getInfo(CL_PROGRAM_DEBUG_INFO_SIZES_INTEL, sizeof(debugDataSize), &debugDataSize, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    std::unique_ptr<char[]> debugData{new char[debugDataSize]};
    for (size_t n = 0; n < sizeof(debugData); n++) {
        debugData[n] = 0;
    }
    char *pDebugData = &debugData[0];
    size_t retData = 0;
    bool isOK = true;
    retVal = pProgram->getInfo(CL_PROGRAM_DEBUG_INFO_INTEL, 1, &pDebugData, &retData);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    retVal = pProgram->getInfo(CL_PROGRAM_DEBUG_INFO_INTEL, debugDataSize, &pDebugData, &retData);
    EXPECT_EQ(CL_SUCCESS, retVal);
    cl_uint numDevices;
    retVal = clGetProgramInfo(pProgram, CL_PROGRAM_NUM_DEVICES, sizeof(numDevices), &numDevices, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(numDevices * sizeof(debugData), retData);
    // Check integrity of returned debug data
    for (size_t n = 0; n < debugDataSize; n++) {
        if (debugData[n] != (char)n) {
            isOK = false;
            break;
        }
    }
    EXPECT_TRUE(isOK);
    for (size_t n = debugDataSize; n < sizeof(debugData); n++) {
        if (debugData[n] != (char)0) {
            isOK = false;
            break;
        }
    }
    EXPECT_TRUE(isOK);

    retData = 0;
    retVal = pProgram->getInfo(CL_PROGRAM_DEBUG_INFO_INTEL, debugDataSize, nullptr, &retData);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(numDevices * sizeof(debugData), retData);
}

TEST_F(ProgramBinTest, GivenDebugDataAvailableWhenLinkingProgramThenDebugDataIsStoredInProgram) {
    DebugDataGuard debugDataGuard;

    cl_device_id device = pClDevice;
    const char *sourceCode = "__kernel void\nCB(\n__global unsigned int* src, __global unsigned int* dst)\n{\nint id = (int)get_global_id(0);\ndst[id] = src[id];\n}\n";
    pProgram = Program::create<MockProgram>(
        pContext,
        1,
        &sourceCode,
        &knownSourceSize,
        retVal);

    retVal = pProgram->compile(1, &device, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    cl_program programToLink = pProgram;
    retVal = pProgram->link(1, &device, nullptr, 1, &programToLink, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);

    EXPECT_NE(nullptr, pProgram->getDebugData());
}

using ProgramMultiRootDeviceTests = MultiRootDeviceFixture;

TEST_F(ProgramMultiRootDeviceTests, WhenPrivateSurfaceIsCreatedThenItHasCorrectRootDeviceIndex) {
    auto program = std::make_unique<MockProgram>(*device->getExecutionEnvironment(), context.get(), false, &device->getDevice());

    auto privateSurfaceBlock = std::make_unique<SPatchAllocateStatelessPrivateSurface>();
    privateSurfaceBlock->DataParamOffset = 0;
    privateSurfaceBlock->DataParamSize = 8;
    privateSurfaceBlock->Size = 8;
    privateSurfaceBlock->SurfaceStateHeapOffset = 0;
    privateSurfaceBlock->Token = 0;
    privateSurfaceBlock->PerThreadPrivateMemorySize = 1000;

    auto infoBlock = std::make_unique<KernelInfo>();
    infoBlock->patchInfo.pAllocateStatelessPrivateSurface = privateSurfaceBlock.get();

    program->blockKernelManager->addBlockKernelInfo(infoBlock.release());
    program->allocateBlockPrivateSurfaces(device->getRootDeviceIndex());

    auto privateSurface = program->getBlockKernelManager()->getPrivateSurface(0);
    EXPECT_NE(nullptr, privateSurface);
    EXPECT_EQ(expectedRootDeviceIndex, privateSurface->getRootDeviceIndex());
}

class MockCompilerInterfaceWithGtpinParam : public CompilerInterface {
  public:
    TranslationOutput::ErrorCode link(
        const NEO::Device &device,
        const TranslationInput &input,
        TranslationOutput &output) override {
        gtpinInfoPassed = input.GTPinInput;
        return CompilerInterface::link(device, input, output);
    }
    void *gtpinInfoPassed;
};

TEST_F(ProgramBinTest, GivenSourceKernelWhenLinkingProgramThenGtpinInitInfoIsPassed) {
    cl_device_id device = pClDevice;
    void *pIgcInitPtr = reinterpret_cast<void *>(0x1234);
    gtpinSetIgcInit(pIgcInitPtr);
    const char *sourceCode = "__kernel void\nCB(\n__global unsigned int* src, __global unsigned int* dst)\n{\nint id = (int)get_global_id(0);\ndst[id] = src[id];\n}\n";
    pProgram = Program::create<MockProgram>(
        pContext,
        1,
        &sourceCode,
        &knownSourceSize,
        retVal);
    std::unique_ptr<MockCompilerInterfaceWithGtpinParam> mockCompilerInterface(new MockCompilerInterfaceWithGtpinParam);

    retVal = pProgram->compile(1, &device, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(CL_SUCCESS, retVal);
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[pDevice->getRootDeviceIndex()]->compilerInterface.reset(mockCompilerInterface.get());

    cl_program programToLink = pProgram;
    retVal = pProgram->link(1, &device, nullptr, 1, &programToLink, nullptr, nullptr);

    EXPECT_EQ(pIgcInitPtr, mockCompilerInterface->gtpinInfoPassed);
    mockCompilerInterface.release();
}

TEST(ProgramReplaceDeviceBinary, GivenBinaryZebinThenUseAsBothPackedAndUnpackedBinaryContainer) {
    MockExecutionEnvironment execEnv;
    ZebinTestData::ValidEmptyProgram zebin;
    std::unique_ptr<char[]> src = makeCopy(zebin.storage.data(), zebin.storage.size());
    MockProgram program{execEnv};
    program.replaceDeviceBinary(std::move(src), zebin.storage.size());
    ASSERT_EQ(zebin.storage.size(), program.packedDeviceBinarySize);
    ASSERT_EQ(zebin.storage.size(), program.unpackedDeviceBinarySize);
    ASSERT_NE(nullptr, program.packedDeviceBinary);
    ASSERT_NE(nullptr, program.unpackedDeviceBinary);
    EXPECT_EQ(0, memcmp(program.packedDeviceBinary.get(), zebin.storage.data(), program.packedDeviceBinarySize));
    EXPECT_EQ(0, memcmp(program.unpackedDeviceBinary.get(), zebin.storage.data(), program.unpackedDeviceBinarySize));
}

TEST(Program, WhenSettingProgramReleaseCallbackThenCallOrderIsPreserved) {
    struct UserDataType {
        cl_program expectedProgram;
        std::vector<size_t> &vectorToModify;
        size_t valueToAdd;
    };
    auto callback = [](cl_program program, void *userData) -> void {
        auto pUserData = reinterpret_cast<UserDataType *>(userData);
        EXPECT_EQ(pUserData->expectedProgram, program);
        pUserData->vectorToModify.push_back(pUserData->valueToAdd);
    };

    MockExecutionEnvironment executionEnvironment;
    auto pProgram = new MockProgram{executionEnvironment};
    std::vector<size_t> callbacksReturnValues;
    UserDataType userDataArray[]{
        {pProgram, callbacksReturnValues, 1},
        {pProgram, callbacksReturnValues, 2},
        {pProgram, callbacksReturnValues, 3}};

    for (auto &userData : userDataArray) {
        cl_int retVal = clSetProgramReleaseCallback(pProgram, callback, &userData);
        ASSERT_EQ(CL_SUCCESS, retVal);
    }
    delete pProgram;

    ASSERT_EQ(3u, callbacksReturnValues.size());
    EXPECT_EQ(3u, callbacksReturnValues[0]);
    EXPECT_EQ(2u, callbacksReturnValues[1]);
    EXPECT_EQ(1u, callbacksReturnValues[2]);
}
