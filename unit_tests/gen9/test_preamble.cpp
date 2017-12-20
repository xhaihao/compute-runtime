/*
 * Copyright (c) 2017, Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "runtime/command_stream/thread_arbitration_policy.h"
#include "runtime/helpers/preamble.h"
#include "runtime/helpers/preamble.inl"
#include "unit_tests/preamble/preamble_fixture.h"
#include "unit_tests/gen_common/gen_cmd_parse.h"

using namespace OCLRT;

typedef PreambleFixture SklSlm;

SKLTEST_F(SklSlm, shouldBeEnabledOnGen9) {
    typedef SKLFamily::MI_LOAD_REGISTER_IMM MI_LOAD_REGISTER_IMM;
    LinearStream &cs = linearStream;
    uint32_t l3Config = PreambleHelper<FamilyType>::getL3Config(**platformDevices, true);

    PreambleHelper<SKLFamily>::programL3(&cs, l3Config);

    parseCommands<SKLFamily>(cs);

    auto itorLRI = find<MI_LOAD_REGISTER_IMM *>(cmdList.begin(), cmdList.end());
    ASSERT_NE(cmdList.end(), itorLRI);

    const auto &lri = *reinterpret_cast<MI_LOAD_REGISTER_IMM *>(*itorLRI);
    auto RegisterOffset = L3CNTLRegisterOffset<FamilyType>::RegisterOffset;
    EXPECT_EQ(RegisterOffset, lri.getRegisterOffset());
    EXPECT_EQ(1u, lri.getDataDword() & 1);
}

typedef PreambleFixture Gen9L3Config;

SKLTEST_F(Gen9L3Config, checkNoSLM) {
    bool slmUsed = false;
    uint32_t l3Config = 0;

    l3Config = getL3ConfigHelper<IGFX_SKYLAKE>(slmUsed);
    EXPECT_EQ(0x80000140u, l3Config);

    l3Config = getL3ConfigHelper<IGFX_BROXTON>(slmUsed);
    EXPECT_EQ(0x80000140u, l3Config);
}

SKLTEST_F(Gen9L3Config, checkSLM) {
    bool slmUsed = true;
    uint32_t l3Config = 0;

    l3Config = getL3ConfigHelper<IGFX_SKYLAKE>(slmUsed);
    EXPECT_EQ(0x60000121u, l3Config);

    l3Config = getL3ConfigHelper<IGFX_BROXTON>(slmUsed);
    EXPECT_EQ(0x60000121u, l3Config);
}

typedef PreambleFixture ThreadArbitration;
SKLTEST_F(ThreadArbitration, givenPreambleWhenItIsProgrammedThenThreadArbitrationIsSetToRoundRobin) {
    typedef SKLFamily::MI_LOAD_REGISTER_IMM MI_LOAD_REGISTER_IMM;
    LinearStream &cs = linearStream;
    uint32_t l3Config = PreambleHelper<FamilyType>::getL3Config(**platformDevices, true);
    PreambleHelper<SKLFamily>::programPreamble(&linearStream, **platformDevices, l3Config, ThreadArbitrationPolicy::threadArbirtrationPolicyRoundRobin);

    parseCommands<SKLFamily>(cs);

    auto itorLRI = reverse_find<MI_LOAD_REGISTER_IMM *>(cmdList.rbegin(), cmdList.rend());
    ASSERT_NE(cmdList.rend(), itorLRI);

    const auto &lri = *reinterpret_cast<MI_LOAD_REGISTER_IMM *>(*itorLRI);
    EXPECT_EQ(0xE404u, lri.getRegisterOffset());
    EXPECT_EQ(0x100u, lri.getDataDword());

    EXPECT_EQ(sizeof(MI_LOAD_REGISTER_IMM), PreambleHelper<SKLFamily>::getAdditionalCommandsSize(**platformDevices));
}

typedef PreambleFixture Gen9UrbEntryAllocationSize;
SKLTEST_F(Gen9UrbEntryAllocationSize, getUrbEntryAllocationSize) {
    uint32_t actualVal = PreambleHelper<FamilyType>::getUrbEntryAllocationSize();
    EXPECT_EQ(0x782u, actualVal);
}