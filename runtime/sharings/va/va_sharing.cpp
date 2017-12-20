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

#include "runtime/context/context.h"
#include "runtime/sharings/sharing_factory.h"
#include "runtime/sharings/va/va_sharing_defines.h"
#include "runtime/sharings/va/va_sharing_functions.h"

namespace OCLRT {
const uint32_t VASharingFunctions::sharingId = SharingType::VA_SHARING;

template <>
VASharingFunctions *Context::getSharing() {
    if (VASharingFunctions::sharingId >= sharingFunctions.size()) {
        DEBUG_BREAK_IF(VASharingFunctions::sharingId >= sharingFunctions.size());
        return nullptr;
    }

    return reinterpret_cast<VASharingFunctions *>(sharingFunctions[VASharingFunctions::sharingId].get());
}
}