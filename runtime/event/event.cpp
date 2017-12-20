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

#include "runtime/command_queue/command_queue.h"
#include "runtime/command_stream/command_stream_receiver.h"
#include "runtime/memory_manager/memory_manager.h"
#include "runtime/context/context.h"
#include "runtime/device/device.h"
#include "runtime/event/event.h"
#include "runtime/helpers/aligned_memory.h"
#include "runtime/helpers/get_info.h"
#include "runtime/api/cl_types.h"
#include "runtime/mem_obj/mem_obj.h"
#include "runtime/utilities/range.h"
#include "runtime/utilities/stackvec.h"
#include "runtime/utilities/tag_allocator.h"
#include "runtime/platform/platform.h"
#include "runtime/event/async_events_handler.h"

namespace OCLRT {

const cl_uint Event::eventNotReady = 0xFFFFFFF0;

Event::Event(
    Context *ctx,
    CommandQueue *cmdQueue,
    cl_command_type cmdType,
    uint32_t taskLevel,
    uint32_t taskCount)
    : taskLevel(taskLevel),
      currentCmdQVirtualEvent(false),
      cmdToSubmit(nullptr),
      submittedCmd(nullptr),
      ctx(ctx),
      cmdQueue(cmdQueue),
      cmdType(cmdType),
      requiresUnregistration(false),
      dataCalculated(false),
      timeStampNode(nullptr),
      perfCounterNode(nullptr),
      perfConfigurationData(nullptr),
      taskCount(taskCount) {
    parentCount = 0;
    executionStatus = CL_QUEUED;
    flushStamp.reset(new FlushStampTracker(true));

    DBG_LOG(EventsDebugEnable, "Event()", this);

    // Event can live longer than command queue that created it,
    // hence command queue refCount must be incremented
    // non-null command queue is only passed when Base Event object is created
    // any other Event types must increment refcount when setting command queue
    if (cmdQueue != nullptr) {
        cmdQueue->incRefInternal();
    }

    if ((this->ctx == nullptr) && (cmdQueue != nullptr)) {
        this->ctx = &cmdQueue->getContext();
    }

    if (this->ctx != nullptr) {
        this->ctx->incRefInternal();
    }

    queueTimeStamp = {0, 0};
    submitTimeStamp = {0, 0};
    startTimeStamp = 0;
    endTimeStamp = 0;
    completeTimeStamp = 0;

    profilingEnabled = !isUserEvent() &&
                       (cmdQueue ? cmdQueue->getCommandQueueProperties() & CL_QUEUE_PROFILING_ENABLE : false);
    profilingCpuPath = ((cmdType == CL_COMMAND_MAP_BUFFER) || (cmdType == CL_COMMAND_MAP_IMAGE)) && profilingEnabled;

    perfCountersEnabled = cmdQueue ? cmdQueue->isPerfCountersEnabled() : false;
}

Event::Event(
    CommandQueue *cmdQueue,
    cl_command_type cmdType,
    uint32_t taskLevel,
    uint32_t taskCount)
    : Event(nullptr, cmdQueue, cmdType, taskLevel, taskCount) {
}

Event::~Event() {
    try {
        DBG_LOG(EventsDebugEnable, "~Event()", this);
        submitCommand(true);

        int32_t lastStatus = executionStatus;
        if (peekIsCompleted(&lastStatus) == false) {
            transitionExecutionStatus(-1);
        }

        // Note from OCL spec:
        //    "All callbacks registered for an event object must be called.
        //     All enqueued callbacks shall be called before the event object is destroyed."
        if (peekHasCallbacks()) {
            executeCallbacks(lastStatus);
        }

        {
            // clean-up submitted command if needed
            std::unique_ptr<Command> submittedCommand(submittedCmd.exchange(nullptr));
        }

        if (cmdQueue != nullptr) {
            cmdQueue->decRefInternal();
        }

        if (ctx != nullptr) {
            if (requiresUnregistration) {
                // in case Event was not properly unregistered prior to destruction
                // we need to remove it from registry to guard it from being corrupted
                ctx->getEventsRegistry().dropEvent(*this);
            }
            if (timeStampNode != nullptr) {
                TagAllocator<HwTimeStamps> *allocator = ctx->getDevice(0)->getMemoryManager()->getEventTsAllocator();
                allocator->returnTag(timeStampNode);
            }
            if (perfCounterNode != nullptr) {
                TagAllocator<HwPerfCounter> *allocator = ctx->getDevice(0)->getMemoryManager()->getEventPerfCountAllocator();
                allocator->returnTag(perfCounterNode);
            }
            ctx->decRefInternal();
        }
        if (perfConfigurationData) {
            delete perfConfigurationData;
        }

        // in case event did not unblock child events before
        unblockEventsBlockedByThis(executionStatus);
    } catch (...) //Don't throw from destructor
    {
    }
}

cl_int Event::getEventProfilingInfo(cl_profiling_info paramName,
                                    size_t paramValueSize,
                                    void *paramValue,
                                    size_t *paramValueSizeRet) {
    if (paramValueSize < sizeof(cl_ulong) && paramValue)
        return CL_INVALID_VALUE;

    if (paramValueSizeRet) {
        *paramValueSizeRet = sizeof(cl_ulong);
    }

    // CL_PROFILING_INFO_NOT_AVAILABLE if event refers to the clEnqueueSVMFree command
    if (isUserEvent() != CL_FALSE || // or is a user event object.
        !peekIsCompleted() ||        // if the execution status of the command identified by event is not CL_COMPLETE
        !profilingEnabled)           // the CL_QUEUE_PROFILING_ENABLE flag is not set for the command-queue,
    {
        return CL_PROFILING_INFO_NOT_AVAILABLE;
    }

    // if paramValue is NULL, it is ignored
    if (paramValue) {
        switch (paramName) {
        case CL_PROFILING_COMMAND_QUEUED:
            *((uint64_t *)paramValue) = queueTimeStamp.CPUTimeinNS;
            return CL_SUCCESS;

        case CL_PROFILING_COMMAND_SUBMIT:
            *((uint64_t *)paramValue) = submitTimeStamp.CPUTimeinNS;
            return CL_SUCCESS;

        case CL_PROFILING_COMMAND_START:
            calcProfilingData();
            *((uint64_t *)paramValue) = startTimeStamp;
            return CL_SUCCESS;

        case CL_PROFILING_COMMAND_END:
            calcProfilingData();
            *((uint64_t *)paramValue) = endTimeStamp;
            return CL_SUCCESS;

        case CL_PROFILING_COMMAND_COMPLETE:
            calcProfilingData();
            *((uint64_t *)paramValue) = completeTimeStamp;
            return CL_SUCCESS;
#if defined(CL_PROFILING_COMMAND_PERFCOUNTERS_INTEL)
        case CL_PROFILING_COMMAND_PERFCOUNTERS_INTEL:
            if (!perfCountersEnabled) {
                return CL_INVALID_VALUE;
            }
            if (!cmdQueue->getPerfCounters()->processEventReport(paramValueSize,
                                                                 paramValue,
                                                                 paramValueSizeRet,
                                                                 getHwPerfCounter(),
                                                                 perfConfigurationData,
                                                                 peekIsCompleted())) {
                return CL_PROFILING_INFO_NOT_AVAILABLE;
            }
            return CL_SUCCESS;
#endif
        default:
            return CL_INVALID_VALUE;
        }
    }
    return CL_SUCCESS;
} // namespace OCLRT

uint32_t Event::getCompletionStamp() const {
    return this->taskCount;
}

void Event::updateCompletionStamp(uint32_t taskCount, uint32_t tasklevel, FlushStamp flushStamp) {
    this->taskCount = taskCount;
    this->taskLevel = tasklevel;
    this->flushStamp->setStamp(flushStamp);
}

cl_ulong Event::getDelta(cl_ulong startTime,
                         cl_ulong endTime) {
    cl_ulong Max = (1ULL << OCLRT_NUM_TIMESTAMP_BITS) - 1;
    cl_ulong Delta = 0;

    startTime &= Max;
    endTime &= Max;

    if (startTime > endTime) {
        Delta = Max - startTime;
        Delta += endTime;
    } else {
        Delta = endTime - startTime;
    }

    return Delta;
}

bool Event::calcProfilingData() {
    uint64_t gpuDuration = 0;
    uint64_t cpuDuration = 0;

    uint64_t gpuCompleteDuration = 0;
    uint64_t cpuCompleteDuration = 0;

    int64_t c0 = 0;
    if (!dataCalculated && timeStampNode && !profilingCpuPath) {
        double frequency = cmdQueue->getDevice().getProfilingTimerResolution();
        /* calculation based on equation
           CpuTime = GpuTime * scalar + const( == c0)
           scalar = DeltaCpu( == dCpu) / DeltaGpu( == dGpu)
           to determine the value of the const we can use one pair of values
           const = CpuTimeQueue - GpuTimeQueue * scalar
        */

        //If device enqueue has not updated complete timestamp, assign end timestamp
        if (((HwTimeStamps *)timeStampNode->tag)->ContextCompleteTS == 0)
            ((HwTimeStamps *)timeStampNode->tag)->ContextCompleteTS = ((HwTimeStamps *)timeStampNode->tag)->ContextEndTS;

        c0 = queueTimeStamp.CPUTimeinNS - static_cast<uint64_t>(queueTimeStamp.GPUTimeStamp * frequency);
        gpuDuration = getDelta(
            ((HwTimeStamps *)timeStampNode->tag)->ContextStartTS,
            ((HwTimeStamps *)timeStampNode->tag)->ContextEndTS);
        gpuCompleteDuration = getDelta(
            ((HwTimeStamps *)timeStampNode->tag)->ContextStartTS,
            ((HwTimeStamps *)timeStampNode->tag)->ContextCompleteTS);
        cpuDuration = static_cast<uint64_t>(gpuDuration * frequency);
        cpuCompleteDuration = static_cast<uint64_t>(gpuCompleteDuration * frequency);
        startTimeStamp = static_cast<uint64_t>(((HwTimeStamps *)timeStampNode->tag)->GlobalStartTS * frequency) + c0;
        endTimeStamp = startTimeStamp + cpuDuration;
        completeTimeStamp = startTimeStamp + cpuCompleteDuration;
        dataCalculated = true;
    }
    return dataCalculated;
}

inline bool Event::wait(bool blocking) {
    while (this->taskCount == Event::eventNotReady) {
        if (blocking == false) {
            return false;
        }
    }

    cmdQueue->waitUntilComplete(taskCount.load(), flushStamp->peekStamp());
    updateExecutionStatus();

    DEBUG_BREAK_IF(this->taskLevel == Event::eventNotReady && this->executionStatus >= 0);

    auto *memoryManager = cmdQueue->getDevice().getMemoryManager();
    memoryManager->cleanAllocationList(this->taskCount, TEMPORARY_ALLOCATION);

    return true;
}

void Event::updateExecutionStatus() {
    if (taskLevel == Event::eventNotReady) {
        return;
    }

    int32_t statusSnapshot = executionStatus;
    if (peekIsCompleted(&statusSnapshot)) {
        executeCallbacks(statusSnapshot);
        return;
    }

    if (peekIsBlocked()) {
        transitionExecutionStatus(CL_QUEUED);
        executeCallbacks(CL_QUEUED);
        return;
    }

    if (statusSnapshot == CL_QUEUED) {
        bool abortBlockedTasks = peekIsCompletedByTermination(&statusSnapshot);
        submitCommand(abortBlockedTasks);
        transitionExecutionStatus(CL_SUBMITTED);
        executeCallbacks(CL_SUBMITTED);
        unblockEventsBlockedByThis(CL_SUBMITTED);
        // Note : Intentional fallthrough (no return) to check for CL_COMPLETE
    }

    if ((cmdQueue != nullptr) && (cmdQueue->isCompleted(getCompletionStamp()))) {
        transitionExecutionStatus(CL_COMPLETE);
        executeCallbacks(CL_COMPLETE);
        unblockEventsBlockedByThis(CL_COMPLETE);
        cmdQueue->getDevice().getMemoryManager()->cleanAllocationList(this->taskCount, TEMPORARY_ALLOCATION);
        return;
    }

    transitionExecutionStatus(CL_SUBMITTED);
}

void Event::addChild(Event &childEvent) {
    childEvent.parentCount++;
    childEvent.incRefInternal();
    childEventsToNotify.pushRefFrontOne(childEvent);
    DBG_LOG(EventsDebugEnable, "addChild: Parent event:", this, "child:", &childEvent);
    if (DebugManager.flags.TrackParentEvents.get()) {
        childEvent.parentEvents.push_back(this);
    }
    if (executionStatus == CL_COMPLETE) {
        unblockEventsBlockedByThis(CL_COMPLETE);
    }
}

void Event::unblockEventsBlockedByThis(int32_t transitionStatus) {

    int32_t status = transitionStatus;
    (void)status;
    DEBUG_BREAK_IF(!(peekIsCompleted(&status) || (peekIsSubmitted(&status))));

    uint32_t taskLevelToPropagate = Event::eventNotReady;

    if (peekIsCompletedByTermination(&transitionStatus) == false) {
        //if we are event on top of the tree , obtain taskLevel from CSR
        if (taskLevel == Event::eventNotReady) {
            this->taskLevel = getTaskLevel();
            taskLevelToPropagate = this->taskLevel;
        } else {
            taskLevelToPropagate = taskLevel + 1;
        }
    }

    auto childEventRef = childEventsToNotify.detachNodes();

    while (childEventRef != nullptr) {
        auto childEvent = childEventRef->ref;

        childEvent->unblockEventBy(*this, taskLevelToPropagate, transitionStatus);

        if (childEvent->getCommandQueue() && childEvent->isCurrentCmdQVirtualEvent()) {
            // Check virtual event state and delete it if possible.
            childEvent->getCommandQueue()->isQueueBlocked();
        }

        childEvent->decRefInternal();
        auto next = childEventRef->next;
        delete childEventRef;
        childEventRef = next;
    }
}

bool Event::setStatus(cl_int status) {
    int32_t prevStatus = executionStatus;

    DBG_LOG(EventsDebugEnable, "setStatus event", this, " new status", status, "previousStatus", prevStatus);

    if (peekIsCompleted(&prevStatus)) {
        return false;
    }

    if (status == prevStatus) {
        return false;
    }

    if (peekIsBlocked() && (peekIsCompletedByTermination(&status) == false)) {
        return false;
    }

    if ((status == CL_SUBMITTED) || (peekIsCompleted(&status))) {
        bool abortBlockedTasks = peekIsCompletedByTermination(&status);
        submitCommand(abortBlockedTasks);
    }

    this->incRefInternal();
    transitionExecutionStatus(status);
    if (peekIsCompleted(&status) || (status == CL_SUBMITTED)) {
        unblockEventsBlockedByThis(status);
    }
    executeCallbacks(status);
    this->decRefInternal();
    return true;
}

void Event::submitCommand(bool abortTasks) {
    std::unique_ptr<Command> cmdToProcess(cmdToSubmit.exchange(nullptr));
    if (cmdToProcess.get() != nullptr) {
        if ((this->isProfilingEnabled()) && (this->cmdQueue != nullptr)) {
            if (timeStampNode) {
                this->cmdQueue->getDevice().getCommandStreamReceiver().makeResident(*timeStampNode->getGraphicsAllocation());
                cmdToProcess->timestamp = timeStampNode->tag;
            }
            if (profilingCpuPath) {
                setSubmitTimeStamp();
                setStartTimeStamp();
            } else {
                this->cmdQueue->getDevice().getOSTime()->getCpuGpuTime(&submitTimeStamp);
            }
            if (perfCountersEnabled && perfCounterNode) {
                this->cmdQueue->getDevice().getCommandStreamReceiver().makeResident(*perfCounterNode->getGraphicsAllocation());
            }
        }
        auto &complStamp = cmdToProcess->submit(taskLevel, abortTasks);
        if (profilingCpuPath && this->isProfilingEnabled() && (this->cmdQueue != nullptr)) {
            setEndTimeStamp();
        }
        updateTaskCount(complStamp.taskCount);
        flushStamp->setStamp(complStamp.flushStamp);
        std::unique_ptr<Command> prevSubmittedCmd;
        prevSubmittedCmd.reset(submittedCmd.exchange(cmdToProcess.release()));
    } else if (profilingCpuPath && endTimeStamp == 0) {
        setEndTimeStamp();
    }
    if (this->taskCount == Event::eventNotReady) {
        if (!this->isUserEvent() && this->eventWithoutCommand) {
            if (this->cmdQueue) {
                TakeOwnershipWrapper<Device> deviceOwnerhsip(this->cmdQueue->getDevice());
                updateTaskCount(this->cmdQueue->getDevice().getCommandStreamReceiver().peekTaskCount());
            }
        }
    }
}

cl_int Event::waitForEvents(cl_uint numEvents,
                            const cl_event *eventList) {
    if (numEvents == 0) {
        return CL_SUCCESS;
    }

    //flush all command queues
    for (const cl_event *it = eventList, *end = eventList + numEvents; it != end; ++it) {
        Event *event = castToObjectOrAbort<Event>(*it);
        if (event->cmdQueue) {
            if (event->taskLevel != Event::eventNotReady) {
                event->cmdQueue->flush();
            }
        }
    }

    using WorkerListT = StackVec<cl_event, 64>;
    WorkerListT workerList1(eventList, eventList + numEvents);
    WorkerListT workerList2;
    workerList2.reserve(numEvents);

    // pointers to workerLists - for fast swap operations
    WorkerListT *currentlyPendingEvents = &workerList1;
    WorkerListT *pendingEventsLeft = &workerList2;

    while (currentlyPendingEvents->size() > 0) {
        for (auto &e : *currentlyPendingEvents) {
            Event *event = castToObjectOrAbort<Event>(e);
            if (event->peekExecutionStatus() < CL_COMPLETE) {
                return CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST;
            }

            if (event->wait(false) == false) {
                pendingEventsLeft->push_back(event);
            }
        }

        if (currentlyPendingEvents->size() == pendingEventsLeft->size()) {
            // we're stuck because of some non-trivial (non-explicit) event dependencies
            // (e.g. event will be signaled from within a callback of an unrelated event)
            Event *baseEvent = castToObjectOrAbort<Event>((*currentlyPendingEvents)[0]);
            Context *ctx = baseEvent->ctx;
            if (ctx == nullptr) {
                DEBUG_BREAK_IF(true);
                return CL_INVALID_CONTEXT;
            }
        }

        std::swap(currentlyPendingEvents, pendingEventsLeft);
        pendingEventsLeft->clear();
    }

    return CL_SUCCESS;
}

uint32_t Event::getTaskLevel() {
    return taskLevel;
}

inline void Event::unblockEventBy(Event &event, uint32_t taskLevel, int32_t transitionStatus) {
    int32_t numEventsBlockingThis = --parentCount;
    DEBUG_BREAK_IF(numEventsBlockingThis < 0);

    int32_t blockerStatus = transitionStatus;
    DEBUG_BREAK_IF(!(peekIsCompleted(&blockerStatus) || peekIsSubmitted(&blockerStatus)));

    if ((numEventsBlockingThis > 0) && (peekIsCompletedByTermination(&blockerStatus) == false)) {
        return;
    }
    DBG_LOG(EventsDebugEnable, "Event", this, "is unblocked by", &event);

    this->taskLevel = taskLevel;

    int32_t statusToPropagate = CL_SUBMITTED;
    if (peekIsCompletedByTermination(&blockerStatus)) {
        statusToPropagate = blockerStatus;
    }
    setStatus(statusToPropagate);

    //event may be completed after this operation, transtition the state to not block others.
    this->updateExecutionStatus();
}

bool Event::isReadyForSubmission() {
    return taskLevel != Event::eventNotReady ? true : false;
}

void Event::addCallback(Callback::ClbFuncT fn, cl_int type, void *data) {
    ECallbackTarget target = translateToCallbackTarget(type);
    if (target == ECallbackTarget::Invalid) {
        DEBUG_BREAK_IF(true);
        return;
    }
    incRefInternal();

    // Note from spec :
    //    "All callbacks registered for an event object must be called.
    //     All enqueued callbacks shall be called before the event object is destroyed."
    // That's why each registered calback increments the internal refcount
    try {
        incRefInternal();
        DBG_LOG(EventsDebugEnable, "event", this, "addCallback", "ECallbackTarget", (uint32_t)type);
        callbacks[(uint32_t)target].pushFrontOne(*new Callback(this, fn, type, data));
    } catch (...) {
        decRefInternal(); // in case we fail adding callback, we don't want to contaminate
                          // the internal ref counter
        throw;
    }

    // Callback added after event reached its "completed" state
    if (peekIsCompleted()) {
        int32_t status = executionStatus;
        DBG_LOG(EventsDebugEnable, "event", this, "addCallback executing callbacks with status", status);
        executeCallbacks(status);
    }

    if (peekHasCallbacks() && !isUserEvent() && DebugManager.flags.EnableAsyncEventsHandler.get()) {
        platform()->getAsyncEventsHandler()->registerEvent(this);
    }

    decRefInternal();
}

void Event::executeCallbacks(int32_t executionStatusIn) {
    int32_t execStatus = executionStatusIn;
    bool terminated = peekIsCompletedByTermination(&execStatus);
    ECallbackTarget target;
    if (terminated) {
        target = ECallbackTarget::Completed;
    } else {
        target = translateToCallbackTarget(execStatus);
        if (target == ECallbackTarget::Invalid) {
            DEBUG_BREAK_IF(true);
            return;
        }
    }

    // run through all needed callback targets and execute callbacks
    for (uint32_t i = 0; i <= (uint32_t)target; ++i) {
        auto cb = callbacks[i].detachNodes();
        auto curr = cb;
        while (curr != nullptr) {
            auto next = curr->next;
            if (terminated) {
                curr->overrideCallbackExecutionStatusTarget(execStatus);
            }
            DBG_LOG(EventsDebugEnable, "event", this, "executing callback", "ECallbackTarget", (uint32_t)target);
            curr->execute();
            decRefInternal();
            delete curr;
            curr = next;
        }
    }
}

void Event::tryFlushEvent() {
    //only if event is not completed, completed event has already been flushed
    if (cmdQueue && (peekIsCompleted() == false)) {
        //flush the command queue only if it is not blocked event
        if (taskLevel != Event::eventNotReady) {
            cl_event ev = this;
            DBG_LOG(EventsDebugEnable, "tryFlushEvent", this);
            cmdQueue->flushWaitList(1, &ev, this->getCommandType() == CL_COMMAND_NDRANGE_KERNEL);
        }
    }
}

void Event::unregisterEvent(Event *ev) {
    if (ev == nullptr) {
        return;
    }
    if (ev->ctx != nullptr) {
        ev->requiresUnregistration = false;
        ev->ctx->getEventsRegistry().unregisterEvent(std::unique_ptr<Event>(ev));
    } else {
        delete ev;
    }
}

void Event::setQueueTimeStamp() {
    if (this->profilingEnabled && (this->cmdQueue != nullptr)) {
        this->cmdQueue->getDevice().getOSTime()->getCpuTime(&queueTimeStamp.CPUTimeinNS);
    }
}

void Event::setSubmitTimeStamp() {
    if (this->profilingEnabled && (this->cmdQueue != nullptr)) {
        this->cmdQueue->getDevice().getOSTime()->getCpuTime(&submitTimeStamp.CPUTimeinNS);
    }
}

void Event::setStartTimeStamp() {
    if (this->profilingEnabled && (this->cmdQueue != nullptr)) {
        this->cmdQueue->getDevice().getOSTime()->getCpuTime(&startTimeStamp);
    }
}

void Event::setEndTimeStamp() {
    if (this->profilingEnabled && (this->cmdQueue != nullptr)) {
        this->cmdQueue->getDevice().getOSTime()->getCpuTime(&endTimeStamp);
        completeTimeStamp = endTimeStamp;
    }
}

HwTimeStamps *Event::getHwTimeStamp() {
    TagNode<HwTimeStamps> *node = nullptr;
    if (!timeStampNode) {
        TagAllocator<HwTimeStamps> *allocator = getCommandQueue()->getDevice().getMemoryManager()->getEventTsAllocator();
        timeStampNode = allocator->getTag();
        timeStampNode->tag->GlobalStartTS = 0;
        timeStampNode->tag->ContextStartTS = 0;
        timeStampNode->tag->GlobalEndTS = 0;
        timeStampNode->tag->ContextEndTS = 0;
        timeStampNode->tag->GlobalCompleteTS = 0;
        timeStampNode->tag->ContextCompleteTS = 0;
    }
    node = timeStampNode;
    return node->tag;
}

GraphicsAllocation *Event::getHwTimeStampAllocation() {
    GraphicsAllocation *gfxalloc = nullptr;
    if (!timeStampNode) {
        TagAllocator<HwTimeStamps> *allocator = getCommandQueue()->getDevice().getMemoryManager()->getEventTsAllocator();
        timeStampNode = allocator->getTag();
    }
    gfxalloc = timeStampNode->getGraphicsAllocation();

    return gfxalloc;
}

HwPerfCounter *Event::getHwPerfCounter() {
    TagNode<HwPerfCounter> *node = nullptr;
    if (!perfCounterNode) {
        TagAllocator<HwPerfCounter> *allocator = getCommandQueue()->getDevice().getMemoryManager()->getEventPerfCountAllocator();
        perfCounterNode = allocator->getTag();
        memset(perfCounterNode->tag, 0, sizeof(HwPerfCounter));
    }
    node = perfCounterNode;
    return node->tag;
}

GraphicsAllocation *Event::getHwPerfCounterAllocation() {
    GraphicsAllocation *gfxalloc = nullptr;
    if (!perfCounterNode) {
        TagAllocator<HwPerfCounter> *allocator = getCommandQueue()->getDevice().getMemoryManager()->getEventPerfCountAllocator();
        perfCounterNode = allocator->getTag();
    }
    gfxalloc = perfCounterNode->getGraphicsAllocation();

    return gfxalloc;
}

void Event::copyPerfCounters(InstrPmRegsCfg *config) {
    perfConfigurationData = new InstrPmRegsCfg;
    memcpy_s(perfConfigurationData, sizeof(InstrPmRegsCfg), config, sizeof(InstrPmRegsCfg));
}

void Event::addToRegistry() {
    UNRECOVERABLE_IF(requiresUnregistration);
    if (this->ctx != nullptr) {
        requiresUnregistration = true;
        this->ctx->getEventsRegistry().registerEvent(*this);
    }
}
} // namespace OCLRT