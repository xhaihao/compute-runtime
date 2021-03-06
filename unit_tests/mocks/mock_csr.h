/*
 * Copyright (c) 2018, Intel Corporation
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

#pragma once
#include "runtime/command_stream/command_stream_receiver.h"
#include "runtime/command_stream/command_stream_receiver_hw.h"
#include "runtime/memory_manager/graphics_allocation.h"
#include "runtime/helpers/options.h"
#include "runtime/helpers/flush_stamp.h"
#include "runtime/helpers/string.h"
#include "unit_tests/libult/ult_command_stream_receiver.h"
#include <vector>

using namespace OCLRT;

template <typename GfxFamily>
class MockCsrBase : public UltCommandStreamReceiver<GfxFamily> {
  public:
    using BaseUltCsrClass = UltCommandStreamReceiver<GfxFamily>;

    MockCsrBase() = delete;

    MockCsrBase(int32_t &execStamp)
        : BaseUltCsrClass(*platformDevices[0]), executionStamp(&execStamp), flushTaskStamp(-1) {
    }

    MockCsrBase(const HardwareInfo &hwInfoIn) : BaseUltCsrClass(hwInfoIn) {
    }

    void makeResident(GraphicsAllocation &gfxAllocation) override {
        madeResidentGfxAllocations.push_back(&gfxAllocation);
        if (this->getMemoryManager()) {
            this->getMemoryManager()->getResidencyAllocations().push_back(&gfxAllocation);
        }
        gfxAllocation.residencyTaskCount = this->taskCount;
    }
    void makeNonResident(GraphicsAllocation &gfxAllocation) override {
        madeNonResidentGfxAllocations.push_back(&gfxAllocation);
    }

    uint32_t peekThreadArbitrationPolicy() { return this->requiredThreadArbitrationPolicy; }

    bool isMadeResident(GraphicsAllocation *gfxAllocation) {
        for (GraphicsAllocation *gfxAlloc : madeResidentGfxAllocations) {
            if (gfxAlloc == gfxAllocation)
                return true;
        }
        return false;
    }

    bool isMadeNonResident(GraphicsAllocation *gfxAllocation) {
        for (GraphicsAllocation *gfxAlloc : madeNonResidentGfxAllocations) {
            if (gfxAlloc == gfxAllocation)
                return true;
        }
        return false;
    }

    bool getGSBAFor32BitProgrammed() {
        return this->GSBAFor32BitProgrammed;
    }

    void processEviction() override {
        processEvictionCalled = true;
    }

    ResidencyContainer madeResidentGfxAllocations;
    ResidencyContainer madeNonResidentGfxAllocations;
    int32_t *executionStamp;
    int32_t flushTaskStamp;
    bool processEvictionCalled = false;
};

template <typename GfxFamily>
using MockCsrHw = MockCsrBase<GfxFamily>;

template <typename GfxFamily>
class MockCsr : public MockCsrBase<GfxFamily> {
  public:
    using BaseClass = MockCsrBase<GfxFamily>;
    using CommandStreamReceiver::mediaVfeStateDirty;

    MockCsr() = delete;
    MockCsr(const HardwareInfo &hwInfoIn) = delete;
    MockCsr(int32_t &execStamp) : BaseClass(execStamp) {
    }

    FlushStamp flush(BatchBuffer &batchBuffer, EngineType engineType, ResidencyContainer *allocationsForResidency) override {
        return 0;
    }

    CompletionStamp flushTask(
        LinearStream &commandStream,
        size_t commandStreamStart,
        const LinearStream &dsh,
        const LinearStream &ih,
        const LinearStream &ioh,
        const LinearStream &ssh,
        uint32_t taskLevel,
        DispatchFlags &dispatchFlags) override {
        this->flushTaskStamp = *this->executionStamp;
        (*this->executionStamp)++;
        slmUsedInLastFlushTask = dispatchFlags.useSLM;
        this->latestSentTaskCount = ++this->taskCount;
        lastTaskLevelToFlushTask = taskLevel;

        return CommandStreamReceiverHw<GfxFamily>::flushTask(
            commandStream,
            commandStreamStart,
            dsh,
            ih,
            ioh,
            ssh,
            taskLevel,
            dispatchFlags);
    }

    bool peekMediaVfeStateDirty() const { return mediaVfeStateDirty; }

    bool slmUsedInLastFlushTask = false;
    uint32_t lastTaskLevelToFlushTask = 0;
};

template <typename GfxFamily>
struct MockCsrHw2 : public CommandStreamReceiverHw<GfxFamily> {
    using CommandStreamReceiverHw<GfxFamily>::flushStamp;
    using CommandStreamReceiver::commandStream;
    using CommandStreamReceiver::dispatchMode;
    using CommandStreamReceiver::lastSentCoherencyRequest;
    using CommandStreamReceiver::mediaVfeStateDirty;
    using CommandStreamReceiver::taskCount;
    using CommandStreamReceiver::taskLevel;

    MockCsrHw2(const HardwareInfo &hwInfoIn) : CommandStreamReceiverHw<GfxFamily>(hwInfoIn) {}

    SubmissionAggregator *peekSubmissionAggregator() {
        return this->submissionAggregator.get();
    }

    void overrideSubmissionAggregator(SubmissionAggregator *newSubmissionsAggregator) {
        this->submissionAggregator.reset(newSubmissionsAggregator);
    }

    uint64_t peekTotalMemoryUsed() {
        return this->totalMemoryUsed;
    }

    bool peekMediaVfeStateDirty() const { return mediaVfeStateDirty; }

    FlushStamp flush(BatchBuffer &batchBuffer, EngineType engineType,
                     ResidencyContainer *allocationsForResidency) override {
        flushCalledCount++;
        recordedCommandBuffer.batchBuffer = batchBuffer;
        copyOfAllocations.clear();

        if (allocationsForResidency) {
            copyOfAllocations = *allocationsForResidency;
        } else {
            copyOfAllocations = this->getMemoryManager()->getResidencyAllocations();
        }
        flushStamp->setStamp(flushStamp->peekStamp() + 1);
        return flushStamp->peekStamp();
    }

    CompletionStamp flushTask(LinearStream &commandStream, size_t commandStreamStart,
                              const LinearStream &dsh, const LinearStream &ih, const LinearStream &ioh,
                              const LinearStream &ssh, uint32_t taskLevel, DispatchFlags &dispatchFlags) override {
        passedDispatchFlags = dispatchFlags;
        return CommandStreamReceiverHw<GfxFamily>::flushTask(commandStream, commandStreamStart,
                                                             dsh, ih, ioh, ssh, taskLevel, dispatchFlags);
    }

    int flushCalledCount = 0;
    CommandBuffer recordedCommandBuffer;
    ResidencyContainer copyOfAllocations;
    DispatchFlags passedDispatchFlags = {};
};

class MockCommandStreamReceiver : public CommandStreamReceiver {
  public:
    using CommandStreamReceiver::latestSentTaskCount;
    using CommandStreamReceiver::tagAddress;
    std::vector<char> instructionHeapReserveredData;

    ~MockCommandStreamReceiver() {
    }

    FlushStamp flush(BatchBuffer &batchBuffer, EngineType engineType, ResidencyContainer *allocationsForResidency) override;

    CompletionStamp flushTask(
        LinearStream &commandStream,
        size_t commandStreamStart,
        const LinearStream &dsh,
        const LinearStream &ih,
        const LinearStream &ioh,
        const LinearStream &ssh,
        uint32_t taskLevel,
        DispatchFlags &dispatchFlags) override;

    void flushBatchedSubmissions() override {
    }

    void waitForTaskCountWithKmdNotifyFallback(uint32_t taskCountToWait, FlushStamp flushStampToWait) override {
    }

    void addPipeControl(LinearStream &commandStream, bool dcFlush) override {
    }

    void setOSInterface(OSInterface *osInterface);

    size_t getInstructionHeapCmdStreamReceiverReservedSize() const override {
        if (instructionHeapReserveredData.size() == 0) {
            return CommandStreamReceiver::getInstructionHeapCmdStreamReceiverReservedSize();
        }

        return instructionHeapReserveredData.size();
    }

    void initializeInstructionHeapCmdStreamReceiverReservedBlock(LinearStream &ih) const override {
        if (instructionHeapReserveredData.size() == 0) {
            CommandStreamReceiver::initializeInstructionHeapCmdStreamReceiverReservedBlock(ih);
            return;
        }

        void *block = ih.getSpace(instructionHeapReserveredData.size());
        memcpy_s(block, instructionHeapReserveredData.size(),
                 instructionHeapReserveredData.data(), instructionHeapReserveredData.size());
    }
};
