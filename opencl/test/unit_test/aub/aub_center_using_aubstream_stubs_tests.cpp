/*
 * Copyright (C) 2018-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/helpers/options.h"
#include "shared/test/unit_test/helpers/debug_manager_state_restore.h"
#include "shared/test/unit_test/helpers/default_hw_info.h"
#include "shared/test/unit_test/helpers/variable_backup.h"

#include "opencl/test/unit_test/mocks/mock_aub_center.h"

#include "gtest/gtest.h"
#include "third_party/aub_stream/headers/aubstream.h"

using namespace NEO;

namespace aub_stream_stubs {
extern uint16_t tbxServerPort;
extern std::string tbxServerIp;
extern bool tbxFrontdoorMode;
} // namespace aub_stream_stubs

TEST(AubCenter, GivenUseAubStreamDebugVariableSetWhenAubCenterIsCreatedThenAubManagerIsNotCreated) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseAubStream.set(true);

    MockAubCenter aubCenter(defaultHwInfo.get(), false, "test", CommandStreamReceiverType::CSR_AUB);

    EXPECT_EQ(nullptr, aubCenter.aubManager.get());
}

TEST(AubCenter, GivenUseAubStreamAndTbxServerIpDebugVariableSetWhenAubCenterIsCreatedThenServerIpIsModified) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseAubStream.set(true);
    DebugManager.flags.TbxServer.set("10.10.10.10");
    VariableBackup<std::string> backup(&aub_stream_stubs::tbxServerIp);

    MockAubCenter aubCenter(defaultHwInfo.get(), false, "", CommandStreamReceiverType::CSR_TBX);

    EXPECT_STREQ("10.10.10.10", aub_stream_stubs::tbxServerIp.c_str());
}

TEST(AubCenter, GivenUseAubStreamAndTbxServerPortDebugVariableSetWhenAubCenterIsCreatedThenServerIpIsModified) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseAubStream.set(true);
    DebugManager.flags.TbxPort.set(1234);

    VariableBackup<uint16_t> backup(&aub_stream_stubs::tbxServerPort);

    uint16_t port = 1234u;
    EXPECT_NE(port, aub_stream_stubs::tbxServerPort);

    MockAubCenter aubCenter(defaultHwInfo.get(), false, "", CommandStreamReceiverType::CSR_TBX);
    EXPECT_EQ(port, aub_stream_stubs::tbxServerPort);
}

TEST(AubCenter, GivenUseAubStreamAndTbxFrontdoorModeDebugVariableSetWhenAubCenterIsCreatedThenFrontdoorModeIsModified) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.UseAubStream.set(true);
    DebugManager.flags.TbxFrontdoorMode.set(true);

    VariableBackup<bool> backup(&aub_stream_stubs::tbxFrontdoorMode);

    EXPECT_FALSE(aub_stream_stubs::tbxFrontdoorMode);

    MockAubCenter aubCenter(defaultHwInfo.get(), false, "", CommandStreamReceiverType::CSR_TBX);
    EXPECT_TRUE(aub_stream_stubs::tbxFrontdoorMode);
}
