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

#include "unit_tests/mocks/mock_csr.h"
#include "runtime/os_interface/os_interface.h"

FlushStamp MockCommandStreamReceiver::flush(BatchBuffer &batchBuffer, EngineType engineOrdinal, ResidencyContainer *allocationsForResidency) {
    FlushStamp stamp = 0;
    return stamp;
}

CompletionStamp MockCommandStreamReceiver::flushTask(
    LinearStream &commandStream,
    size_t commandStreamStart,
    const LinearStream &dsh,
    const LinearStream &ih,
    const LinearStream &ioh,
    const LinearStream &ssh,
    uint32_t taskLevel,
    DispatchFlags &dispatchFlags) {
    ++taskCount;
    CompletionStamp stamp = {taskCount, taskLevel, flushStamp->peekStamp(), 0, 0};
    return stamp;
}

void MockCommandStreamReceiver::setOSInterface(OSInterface *osInterface) {
    this->osInterface.reset(osInterface);
}