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

#include "runtime/helpers/aligned_memory.h"
#include "runtime/helpers/ptr_math.h"
#include "runtime/gmm_helper/gmm_helper.h"
#include "runtime/gmm_helper/resource_info.h"
#include "runtime/helpers/surface_formats.h"
#include "runtime/memory_manager/deferred_deleter.h"
#include "runtime/memory_manager/deferrable_deletion.h"
#include "runtime/os_interface/windows/wddm_memory_manager.h"
#include "runtime/os_interface/windows/wddm_allocation.h"
#include "runtime/os_interface/windows/wddm.h"
#include <algorithm>
#undef max

namespace OCLRT {

WddmMemoryManager::~WddmMemoryManager() {
    applyCommonCleanup();
    delete this->wddm;
}

WddmMemoryManager::WddmMemoryManager(bool enable64kbPages, Wddm *wddm) : MemoryManager(enable64kbPages), residencyLock(false) {
    DEBUG_BREAK_IF(wddm == nullptr);
    this->wddm = wddm;
    allocator32Bit = std::unique_ptr<Allocator32bit>(new Allocator32bit(wddm->getHeap32Base(), wddm->getHeap32Size()));
    wddm->registerTrimCallback(trimCallback, this);
    asyncDeleterEnabled = DebugManager.flags.EnableDeferredDeleter.get();
    if (asyncDeleterEnabled)
        deferredDeleter = createDeferredDeleter();
}

void APIENTRY WddmMemoryManager::trimCallback(_Inout_ D3DKMT_TRIMNOTIFICATION *trimNotification) {

    WddmMemoryManager *wddmMemMngr = (WddmMemoryManager *)trimNotification->Context;
    DEBUG_BREAK_IF(wddmMemMngr == nullptr);

    wddmMemMngr->trimResidency(trimNotification->Flags, trimNotification->NumBytesToTrim);
}

GraphicsAllocation *WddmMemoryManager::allocateGraphicsMemoryForImage(ImageInfo &imgInfo, Gmm *gmm) {
    if (!Gmm::allowTiling(*imgInfo.imgDesc)) {
        delete gmm;
        return allocateGraphicsMemory(imgInfo.size, MemoryConstants::preferredAlignment);
    }

    WddmAllocation allocation(nullptr, imgInfo.size);
    allocation.gmm = gmm;
    bool success = wddm->createAllocation(&allocation);
    allocation.setGpuAddress(allocation.gpuPtr);

    if (success) {
        auto *wddmAllocation = new WddmAllocation(allocation);
        return wddmAllocation;
    }
    return nullptr;
}

GraphicsAllocation *WddmMemoryManager::allocateGraphicsMemory64kb(size_t size, size_t alignment, bool forcePin) {
    size_t sizeAligned = alignUp(size, MemoryConstants::pageSize64k);
    bool success = true;
    Gmm *gmm = nullptr;

    WddmAllocation allocation(nullptr, sizeAligned, nullptr, sizeAligned);

    gmm = Gmm::create(nullptr, sizeAligned);

    while (success) {
        allocation.gmm = gmm;
        success = wddm->createAllocation64k(&allocation);

        if (!success)
            break;

        auto *wddmAllocation = new WddmAllocation(allocation);
        auto cpuPtr = lockResource(wddmAllocation);
        wddmAllocation->setLocked(true);

        wddmAllocation->setAlignedCpuPtr(cpuPtr);
        // 64kb map is not needed
        wddm->mapGpuVirtualAddress(wddmAllocation, cpuPtr, sizeAligned, false, false);
        wddmAllocation->setCpuPtrAndGpuAddress(cpuPtr, (uint64_t)wddmAllocation->gpuPtr);

        return wddmAllocation;
    }

    delete gmm;
    return nullptr;
}

GraphicsAllocation *WddmMemoryManager::allocateGraphicsMemory(size_t size, size_t alignment, bool forcePin) {
    size_t newAlignment = alignment ? alignUp(alignment, MemoryConstants::pageSize) : MemoryConstants::pageSize;
    size_t sizeAligned = size ? alignUp(size, MemoryConstants::pageSize) : MemoryConstants::pageSize;
    void *pSysMem = allocateSystemMemory(sizeAligned, newAlignment);
    bool success = true;
    Gmm *gmm = nullptr;

    if (pSysMem == nullptr) {
        return nullptr;
    }

    WddmAllocation allocation(pSysMem, sizeAligned, pSysMem, sizeAligned);
    allocation.cpuPtrAllocated = true;

    gmm = Gmm::create(pSysMem, sizeAligned);

    while (success) {
        allocation.gmm = gmm;
        success = wddm->createAllocation(&allocation);

        if (!success)
            break;

        auto *wddmAllocation = new WddmAllocation(allocation);
        return wddmAllocation;
    }

    delete gmm;
    freeSystemMemory(pSysMem);
    return nullptr;
}

GraphicsAllocation *WddmMemoryManager::allocateGraphicsMemory(size_t size, const void *ptrArg) {
    void *ptr = const_cast<void *>(ptrArg);

    if (ptr == nullptr) {
        DEBUG_BREAK_IF(true);
        return nullptr;
    }
    return MemoryManager::allocateGraphicsMemory(size, ptr);
}

GraphicsAllocation *WddmMemoryManager::allocate32BitGraphicsMemory(size_t size, void *ptr) {
    GraphicsAllocation *graphicsAllocation = nullptr;
    bool success = true;
    Gmm *gmm = nullptr;
    const void *ptrAligned = nullptr;
    size_t sizeAligned = size;
    void *pSysMem = nullptr;
    size_t offset = 0;
    bool cpuPtrAllocated = false;

    if (ptr) {
        ptrAligned = alignDown(ptr, MemoryConstants::allocationAlignment);
        sizeAligned = alignSizeWholePage(ptr, size);
        offset = ptrDiff(ptr, ptrAligned);
    } else {
        sizeAligned = alignUp(size, MemoryConstants::allocationAlignment);
        pSysMem = allocateSystemMemory(sizeAligned, MemoryConstants::allocationAlignment);
        if (pSysMem == nullptr) {
            return nullptr;
        }
        ptrAligned = pSysMem;
        cpuPtrAllocated = true;
    }

    WddmAllocation allocation((void *)ptrAligned, sizeAligned, (void *)ptrAligned, sizeAligned);
    allocation.cpuPtrAllocated = cpuPtrAllocated;
    allocation.is32BitAllocation = true;

    gmm = Gmm::create(ptrAligned, sizeAligned);

    while (success) {
        allocation.gmm = gmm;
        success = wddm->createAllocation(&allocation);

        if (!success)
            break;

        allocation.setGpuAddress(allocation.gpuPtr);
        allocation.allocationOffset = offset;

        auto *wddmAllocation = new WddmAllocation(allocation);

        graphicsAllocation = wddmAllocation;
        graphicsAllocation->is32BitAllocation = true;
        graphicsAllocation->gpuBaseAddress = Gmm::canonize(allocator32Bit->getBase());

        return graphicsAllocation;
    }

    delete gmm;
    freeSystemMemory(pSysMem);
    return nullptr;
}

GraphicsAllocation *WddmMemoryManager::createAllocationFromHandle(osHandle handle, bool requireSpecificBitness, bool ntHandle) {
    WddmAllocation allocation(nullptr, 0, handle);
    bool is32BitAllocation = false;

    if (ntHandle) {
        wddm->openNTHandle((HANDLE)((UINT_PTR)handle), &allocation);
    } else {
        wddm->openSharedHandle(handle, &allocation);
    }

    // Shared objects are passed without size
    size_t size = allocation.gmm->gmmResourceInfo->getSizeAllocation();
    allocation.setSize(size);

    void *ptr = nullptr;
    if (is32bit) {
        ptr = (void *)VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
    } else if (requireSpecificBitness && this->force32bitAllocations) {
        is32BitAllocation = true;
        allocation.is32BitAllocation = true;
        allocation.gpuBaseAddress = Gmm::canonize(allocator32Bit->getBase());
    }
    wddm->mapGpuVirtualAddress(&allocation, ptr, size, is32BitAllocation, false);

    allocation.setGpuAddress(allocation.gpuPtr);

    auto *wddmAllocation = new WddmAllocation(allocation);
    return wddmAllocation;
}

GraphicsAllocation *WddmMemoryManager::createGraphicsAllocationFromSharedHandle(osHandle handle, bool requireSpecificBitness, bool /*isReused*/) {
    return createAllocationFromHandle(handle, requireSpecificBitness, false);
}

GraphicsAllocation *WddmMemoryManager::createGraphicsAllocationFromNTHandle(void *handle) {
    return createAllocationFromHandle((osHandle)((UINT_PTR)handle), false, true);
}

void *WddmMemoryManager::lockResource(GraphicsAllocation *graphicsAllocation) {
    return wddm->lockResource(static_cast<WddmAllocation *>(graphicsAllocation));
};
void WddmMemoryManager::unlockResource(GraphicsAllocation *graphicsAllocation) {
    wddm->unlockResource(static_cast<WddmAllocation *>(graphicsAllocation));
};

void WddmMemoryManager::freeGraphicsMemoryImpl(GraphicsAllocation *gfxAllocation) {
    WddmAllocation *input = static_cast<WddmAllocation *>(gfxAllocation);
    auto status = validateAllocation(input);
    DEBUG_BREAK_IF(!status);
    acquireResidencyLock();
    if (input->getTrimCandidateListPosition() != trimListUnusedPosition) {
        removeFromTrimCandidateList(gfxAllocation, true);
    }

    releaseResidencyLock();

    if (input->gmm) {
        if (input->gmm->isRenderCompressed) {
            status = unmapAuxVA(input->gmm, input->gpuPtr);
            DEBUG_BREAK_IF(!status);
        }
        delete input->gmm;
    }
    if (input->peekSharedHandle() == false &&
        input->cpuPtrAllocated == false &&
        input->fragmentsStorage.fragmentCount > 0) {
        cleanGraphicsMemoryCreatedFromHostPtr(gfxAllocation);
    } else {
        D3DKMT_HANDLE *allocationHandles = nullptr;
        uint32_t allocationCount = 0;
        D3DKMT_HANDLE resourceHandle = 0;
        void *cpuPtr = nullptr;
        void *gpuPtr = nullptr;
        if (input->peekSharedHandle()) {
            resourceHandle = input->resourceHandle;
            if (is32bit) {
                gpuPtr = (void *)input->gpuPtr;
            }
        } else {
            allocationHandles = &input->handle;
            allocationCount = 1;
            if (input->cpuPtrAllocated) {
                cpuPtr = input->getAlignedCpuPtr();
            }
        }
        if (input->isLocked()) {
            unlockResource(input);
            input->setLocked(false);
        }
        status = tryDeferDeletions(allocationHandles, allocationCount, input->getResidencyData().lastFence, resourceHandle, cpuPtr, gpuPtr);
        DEBUG_BREAK_IF(!status);
    }
    delete gfxAllocation;
}

bool WddmMemoryManager::tryDeferDeletions(D3DKMT_HANDLE *handles, uint32_t allocationCount, uint64_t lastFenceValue, D3DKMT_HANDLE resourceHandle, void *cpuPtr, void *gpuPtr) {
    bool status = true;
    if (deferredDeleter) {
        deferredDeleter->deferDeletion(DeferrableDeletion::create(wddm, handles, allocationCount, lastFenceValue, resourceHandle, cpuPtr, gpuPtr));
    } else {
        status = wddm->destroyAllocations(handles, allocationCount, lastFenceValue, resourceHandle);
        ::alignedFree(cpuPtr);
        wddm->releaseGpuPtr(gpuPtr);
    }
    return status;
}

bool WddmMemoryManager::validateAllocation(WddmAllocation *alloc) {
    if (alloc == nullptr)
        return false;
    auto size = alloc->getUnderlyingBufferSize();
    if (alloc->getGpuAddress() == 0u || size == 0 || (alloc->handle == 0 && alloc->fragmentsStorage.fragmentCount == 0))
        return false;
    return true;
}

bool WddmMemoryManager::populateOsHandles(OsHandleStorage &handleStorage) {
    for (unsigned int i = 0; i < max_fragments_count; i++) {
        // If no fragment is present it means it already exists.
        if (!handleStorage.fragmentStorageData[i].osHandleStorage && handleStorage.fragmentStorageData[i].cpuPtr) {
            handleStorage.fragmentStorageData[i].osHandleStorage = new OsHandle();
            handleStorage.fragmentStorageData[i].residency = new ResidencyData();

            handleStorage.fragmentStorageData[i].osHandleStorage->gmm = Gmm::create(handleStorage.fragmentStorageData[i].cpuPtr, handleStorage.fragmentStorageData[i].fragmentSize);
            hostPtrManager.storeFragment(handleStorage.fragmentStorageData[i]);
        }
    }
    wddm->createAllocationsAndMapGpuVa(handleStorage);
    return true;
}

void WddmMemoryManager::cleanOsHandles(OsHandleStorage &handleStorage) {

    D3DKMT_HANDLE handles[max_fragments_count] = {0};
    auto allocationCount = 0;

    uint64_t lastFenceValue = 0;

    for (unsigned int i = 0; i < max_fragments_count; i++) {
        if (handleStorage.fragmentStorageData[i].freeTheFragment) {
            handles[allocationCount] = handleStorage.fragmentStorageData[i].osHandleStorage->handle;
            handleStorage.fragmentStorageData[i].residency->resident = false;
            allocationCount++;
            lastFenceValue = std::max(handleStorage.fragmentStorageData[i].residency->lastFence, lastFenceValue);
        }
    }

    bool success = tryDeferDeletions(handles, allocationCount, lastFenceValue, 0, nullptr, nullptr);

    for (unsigned int i = 0; i < max_fragments_count; i++) {
        if (handleStorage.fragmentStorageData[i].freeTheFragment) {
            if (success) {
                handleStorage.fragmentStorageData[i].osHandleStorage->handle = 0;
            }
            delete handleStorage.fragmentStorageData[i].osHandleStorage->gmm;
            delete handleStorage.fragmentStorageData[i].osHandleStorage;
            delete handleStorage.fragmentStorageData[i].residency;
        }
    }
}

GraphicsAllocation *WddmMemoryManager::createGraphicsAllocation(OsHandleStorage &handleStorage, size_t hostPtrSize, const void *hostPtr) {
    auto allocation = new WddmAllocation(const_cast<void *>(hostPtr), hostPtrSize, const_cast<void *>(hostPtr), hostPtrSize);
    allocation->fragmentsStorage = handleStorage;
    return allocation;
}

uint64_t WddmMemoryManager::getSystemSharedMemory() {
    return wddm->getSystemSharedMemory();
}

uint64_t WddmMemoryManager::getMaxApplicationAddress() {
    return wddm->getMaxApplicationAddress();
}

bool WddmMemoryManager::makeResidentResidencyAllocations(ResidencyContainer *allocationsForResidency) {

    auto &residencyAllocations = allocationsForResidency ? *allocationsForResidency : this->residencyAllocations;

    size_t residencyCount = residencyAllocations.size();
    std::unique_ptr<D3DKMT_HANDLE[]> handlesForResidency(new D3DKMT_HANDLE[residencyCount * max_fragments_count]);

    uint32_t totalHandlesCount = 0;

    acquireResidencyLock();

    DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "currentFenceValue =", wddm->getMonitoredFence().currentFenceValue);

    for (uint32_t i = 0; i < residencyCount; i++) {
        WddmAllocation *allocation = reinterpret_cast<WddmAllocation *>(residencyAllocations[i]);
        bool mainResidency = false;
        bool fragmentResidency[3] = {false, false, false};

        mainResidency = allocation->getResidencyData().resident;

        DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "allocation =", allocation, mainResidency ? "resident" : "not resident");

        if (allocation->getTrimCandidateListPosition() != trimListUnusedPosition) {

            DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "allocation =", allocation, "on trimCandidateList");
            removeFromTrimCandidateList(allocation);
        } else {

            for (uint32_t allocationId = 0; allocationId < allocation->fragmentsStorage.fragmentCount; allocationId++) {
                fragmentResidency[allocationId] = allocation->fragmentsStorage.fragmentStorageData[allocationId].residency->resident;

                DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "fragment handle =",
                        allocation->fragmentsStorage.fragmentStorageData[allocationId].osHandleStorage->handle,
                        fragmentResidency[allocationId] ? "resident" : "not resident");
            }
        }

        if (allocation->fragmentsStorage.fragmentCount == 0) {
            if (!mainResidency)
                handlesForResidency[totalHandlesCount++] = allocation->handle;
        } else {
            for (uint32_t allocationId = 0; allocationId < allocation->fragmentsStorage.fragmentCount; allocationId++) {
                if (!fragmentResidency[allocationId])
                    handlesForResidency[totalHandlesCount++] = allocation->fragmentsStorage.fragmentStorageData[allocationId].osHandleStorage->handle;
            }
        }
    }

    bool result = true;
    if (totalHandlesCount) {
        uint64_t bytesToTrim = 0;
        while ((result = wddm->makeResident(handlesForResidency.get(), totalHandlesCount, false, &bytesToTrim)) == false) {
            this->memoryBudgetExhausted = true;
            bool trimmingDone = trimResidencyToBudget(bytesToTrim);
            bool cantTrimFurther = !trimmingDone;
            if (cantTrimFurther) {
                result = wddm->makeResident(handlesForResidency.get(), totalHandlesCount, true, &bytesToTrim);
                break;
            }
        }
    }

    if (result == true) {
        for (uint32_t i = 0; i < residencyCount; i++) {
            WddmAllocation *allocation = reinterpret_cast<WddmAllocation *>(residencyAllocations[i]);
            // Update fence value not to early destroy / evict allocation
            allocation->getResidencyData().lastFence = wddm->getMonitoredFence().currentFenceValue;
            allocation->getResidencyData().resident = true;

            for (uint32_t allocationId = 0; allocationId < allocation->fragmentsStorage.fragmentCount; allocationId++) {
                allocation->fragmentsStorage.fragmentStorageData[allocationId].residency->resident = true;
                // Update fence value not to remove the fragment referenced by different GA in trimming callback
                allocation->fragmentsStorage.fragmentStorageData[allocationId].residency->lastFence = wddm->getMonitoredFence().currentFenceValue;
            }
        }
    }

    releaseResidencyLock();

    return result;
}

void WddmMemoryManager::makeNonResidentEvictionAllocations() {

    acquireResidencyLock();

    size_t residencyCount = evictionAllocations.size();

    for (uint32_t i = 0; i < residencyCount; i++) {
        WddmAllocation *allocation = reinterpret_cast<WddmAllocation *>(evictionAllocations[i]);

        addToTrimCandidateList(allocation);
    }

    releaseResidencyLock();
}

void WddmMemoryManager::removeFromTrimCandidateList(GraphicsAllocation *allocation, bool compactList) {
    WddmAllocation *wddmAllocation = (WddmAllocation *)allocation;
    size_t position = wddmAllocation->getTrimCandidateListPosition();

    DEBUG_BREAK_IF(!(trimCandidatesCount > (trimCandidatesCount - 1)));
    DEBUG_BREAK_IF(trimCandidatesCount > trimCandidateList.size());

    trimCandidatesCount--;

    trimCandidateList[position] = nullptr;

    checkTrimCandidateCount();

    if (position == trimCandidateList.size() - 1) {
        size_t erasePosition = position;

        if (position == 0) {
            trimCandidateList.resize(0);
        } else {
            while (trimCandidateList[erasePosition] == nullptr && erasePosition > 0) {
                erasePosition--;
            }

            size_t sizeRemaining = erasePosition + 1;
            if (erasePosition == 0 && trimCandidateList[erasePosition] == nullptr) {
                sizeRemaining = 0;
            }

            trimCandidateList.resize(sizeRemaining);
        }
    }
    wddmAllocation->setTrimCandidateListPosition(trimListUnusedPosition);

    if (compactList && checkTrimCandidateListCompaction()) {
        compactTrimCandidateList();
    }

    checkTrimCandidateCount();
}

void WddmMemoryManager::addToTrimCandidateList(GraphicsAllocation *allocation) {
    WddmAllocation *wddmAllocation = (WddmAllocation *)allocation;
    size_t position = trimCandidateList.size();

    DEBUG_BREAK_IF(trimCandidatesCount > trimCandidateList.size());

    if (wddmAllocation->getTrimCandidateListPosition() == trimListUnusedPosition) {
        trimCandidatesCount++;
        trimCandidateList.push_back(allocation);
        wddmAllocation->setTrimCandidateListPosition(position);
    }

    checkTrimCandidateCount();
}

void WddmMemoryManager::compactTrimCandidateList() {

    size_t size = trimCandidateList.size();
    size_t freePosition = 0;

    if (size == 0 || size == trimCandidatesCount) {
        return;
    }

    DEBUG_BREAK_IF(!(trimCandidateList[size - 1] != nullptr));

    uint32_t previousCount = trimCandidatesCount;
    DEBUG_BREAK_IF(trimCandidatesCount > trimCandidateList.size());

    while (freePosition < trimCandidatesCount && trimCandidateList[freePosition] != nullptr)
        freePosition++;

    for (uint32_t i = 1; i < size; i++) {

        if (trimCandidateList[i] != nullptr && freePosition < i) {
            trimCandidateList[freePosition] = trimCandidateList[i];
            trimCandidateList[i] = nullptr;
            ((WddmAllocation *)trimCandidateList[freePosition])->setTrimCandidateListPosition(freePosition);
            freePosition++;

            // Last element was moved, erase elements from freePosition
            if (i == size - 1) {
                trimCandidateList.resize(freePosition);
            }
        }
    }
    DEBUG_BREAK_IF(trimCandidatesCount > trimCandidateList.size());
    DEBUG_BREAK_IF(trimCandidatesCount != previousCount);

    checkTrimCandidateCount();
}

void WddmMemoryManager::trimResidency(D3DDDI_TRIMRESIDENCYSET_FLAGS flags, uint64_t bytes) {
    if (flags.PeriodicTrim) {
        bool periodicTrimDone = false;
        D3DKMT_HANDLE fragmentEvictHandles[3] = {0};
        uint64_t sizeToTrim = 0;

        acquireResidencyLock();

        size_t size = trimCandidateList.size();

        WddmAllocation *wddmAllocation = nullptr;
        while ((wddmAllocation = getTrimCandidateHead()) != nullptr) {

            DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "lastPeriodicTrimFenceValue = ", lastPeriodicTrimFenceValue);

            // allocation was not used from last periodic trim
            if ((wddmAllocation)->getResidencyData().lastFence <= lastPeriodicTrimFenceValue) {

                DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "allocation: handle =", wddmAllocation->handle, "lastFence =", (wddmAllocation)->getResidencyData().lastFence);

                size_t fragmentsSizeToEvict = 0;
                uint32_t fragmentsToEvict = 0;

                if (wddmAllocation->fragmentsStorage.fragmentCount == 0) {
                    DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "Evict allocation: handle =", wddmAllocation->handle, "lastFence =", (wddmAllocation)->getResidencyData().lastFence);
                    wddm->evict(&wddmAllocation->handle, 1, sizeToTrim);
                }

                for (uint32_t allocationId = 0; allocationId < wddmAllocation->fragmentsStorage.fragmentCount; allocationId++) {
                    if (wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].residency->lastFence <= lastPeriodicTrimFenceValue) {

                        DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "Evict fragment: handle =", wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].osHandleStorage->handle, "lastFence =", wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].residency->lastFence);

                        fragmentEvictHandles[fragmentsToEvict++] = wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].osHandleStorage->handle;
                        wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].residency->resident = false;
                    }
                }

                if (fragmentsToEvict != 0) {
                    wddm->evict((D3DKMT_HANDLE *)fragmentEvictHandles, fragmentsToEvict, sizeToTrim);
                }

                wddmAllocation->getResidencyData().resident = false;
                removeFromTrimCandidateList(wddmAllocation);
            } else {
                periodicTrimDone = true;
                break;
            }
        }

        if (checkTrimCandidateListCompaction()) {
            compactTrimCandidateList();
        }

        releaseResidencyLock();
    }

    if (flags.TrimToBudget) {

        acquireResidencyLock();

        trimResidencyToBudget(bytes);

        releaseResidencyLock();
    }

    if (flags.PeriodicTrim || flags.RestartPeriodicTrim) {
        lastPeriodicTrimFenceValue = *wddm->getMonitoredFence().cpuAddress;
        DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "updated lastPeriodicTrimFenceValue =", lastPeriodicTrimFenceValue);
    }
}

void WddmMemoryManager::checkTrimCandidateCount() {
    if (DebugManager.flags.ResidencyDebugEnable.get()) {
        uint32_t sum = 0;
        for (size_t i = 0; i < trimCandidateList.size(); i++) {
            if (trimCandidateList[i] != nullptr) {
                sum++;
            }
        }
        DEBUG_BREAK_IF(sum != trimCandidatesCount);
    }
}

bool WddmMemoryManager::checkTrimCandidateListCompaction() {
    if (2 * trimCandidatesCount <= trimCandidateList.size()) {
        return true;
    }
    return false;
}

bool WddmMemoryManager::trimResidencyToBudget(uint64_t bytes) {
    bool trimToBudgetDone = false;
    D3DKMT_HANDLE fragmentEvictHandles[3] = {0};
    uint64_t numberOfBytesToTrim = bytes;
    WddmAllocation *wddmAllocation = nullptr;

    trimToBudgetDone = (numberOfBytesToTrim == 0);

    while (!trimToBudgetDone) {
        uint64_t lastFence = 0;
        wddmAllocation = getTrimCandidateHead();

        if (wddmAllocation == nullptr) {
            break;
        }

        lastFence = wddmAllocation->getResidencyData().lastFence;

        if (lastFence <= wddm->getMonitoredFence().lastSubmittedFence) {
            uint32_t fragmentsToEvict = 0;
            uint64_t sizeEvicted = 0;
            uint64_t sizeToTrim = 0;

            if (lastFence > *wddm->getMonitoredFence().cpuAddress) {
                wddm->waitFromCpu(lastFence);
            }

            if (wddmAllocation->fragmentsStorage.fragmentCount == 0) {
                wddm->evict(&wddmAllocation->handle, 1, sizeToTrim);

                sizeEvicted = wddmAllocation->getUnderlyingBufferSize();
            } else {
                for (uint32_t allocationId = 0; allocationId < wddmAllocation->fragmentsStorage.fragmentCount; allocationId++) {
                    if (wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].residency->lastFence <= wddm->getMonitoredFence().lastSubmittedFence) {
                        fragmentEvictHandles[fragmentsToEvict++] = wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].osHandleStorage->handle;
                    }
                }

                if (fragmentsToEvict != 0) {
                    wddm->evict((D3DKMT_HANDLE *)fragmentEvictHandles, fragmentsToEvict, sizeToTrim);

                    for (uint32_t allocationId = 0; allocationId < wddmAllocation->fragmentsStorage.fragmentCount; allocationId++) {
                        if (wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].residency->lastFence <= wddm->getMonitoredFence().lastSubmittedFence) {
                            wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].residency->resident = false;
                            sizeEvicted += wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].fragmentSize;
                        }
                    }
                }
            }

            if (sizeEvicted >= numberOfBytesToTrim) {
                numberOfBytesToTrim = 0;
            } else {
                numberOfBytesToTrim -= sizeEvicted;
            }

            wddmAllocation->getResidencyData().resident = false;
            removeFromTrimCandidateList(wddmAllocation);
            trimToBudgetDone = (numberOfBytesToTrim == 0);
        } else {
            trimToBudgetDone = true;
        }
    }

    if (bytes > numberOfBytesToTrim && checkTrimCandidateListCompaction()) {
        compactTrimCandidateList();
    }

    return numberOfBytesToTrim == 0;
}

bool WddmMemoryManager::unmapAuxVA(Gmm *gmm, D3DGPU_VIRTUAL_ADDRESS &gpuVA) {
    GMM_DDI_UPDATEAUXTABLE ddiUpdateAuxTable = {};
    ddiUpdateAuxTable.BaseGpuVA = gpuVA;
    ddiUpdateAuxTable.BaseResInfo = gmm->gmmResourceInfo->peekHandle();
    ddiUpdateAuxTable.DoNotWait = true;
    ddiUpdateAuxTable.Map = false;
    return wddm->updateAuxTable(ddiUpdateAuxTable);
}
} // namespace OCLRT