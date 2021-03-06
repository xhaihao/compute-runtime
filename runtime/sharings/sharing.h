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

#pragma once
#include "runtime/memory_manager/graphics_allocation.h"

namespace OCLRT {
class Context;
class MemObj;

enum SynchronizeStatus {
    SHARED_OBJECT_NOT_CHANGED,
    SHARED_OBJECT_REQUIRES_UPDATE,
    ACQUIRE_SUCCESFUL,
    SYNCHRONIZE_ERROR
};

struct UpdateData {
    SynchronizeStatus synchronizationStatus;
    osHandle sharedHandle;
    MemObj *memObject = nullptr;
    void *updateData = nullptr;
};

class SharingFunctions {
  public:
    virtual uint32_t getId() const = 0;

    virtual ~SharingFunctions() = default;
};

class SharingHandler {
  public:
    void acquire(MemObj *memObj);
    void release(MemObj *memObject);
    virtual ~SharingHandler() = default;

    virtual void getMemObjectInfo(size_t &paramValueSize, void *&paramValue){};

  protected:
    virtual void synchronizeHandler(UpdateData *updateData);
    virtual void synchronizeObject(UpdateData *updateData) { updateData->synchronizationStatus = SYNCHRONIZE_ERROR; }
    virtual void releaseResource(MemObj *memObject){};
    unsigned int acquireCount = 0u;
};
} // namespace OCLRT
