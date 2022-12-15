#include <sys/mman.h>
#include "spdlog/spdlog.h"

#include "src/common/include/configs.h"
#include "src/common/include/exception.h"
#include "src/common/include/utils.h"
#include "src/storage/buffer_manager/include/buffer_manager.h"

using namespace kuzu::common;

namespace kuzu {
namespace storage {

Frame::Frame(uint64_t pageSize) : frameLock{ATOMIC_FLAG_INIT}, pageSize{pageSize}, eviction_timestamp(0) {
    resetFrameWithoutLock();
    buffer = make_unique<uint8_t[]>(pageSize);
}

Frame::Frame(uint64_t pageSize, uint8_t* buffer) : frameLock{ATOMIC_FLAG_INIT}, pageSize{pageSize}, eviction_timestamp(0) {
    resetFrameWithoutLock();
    mmapBuffer = buffer;
}

Frame::~Frame() noexcept(false) {
    auto count = pinCount.load();
    if (0 != count && -1u != count) {
        throw BufferManagerException("Deleting buffer that is still pinned. pinCount: " +
                                     to_string(count) + " pageIdx: " + to_string(pageIdx));
    }
}

void Frame::releaseMemory() {
    int error = madvise(mmapBuffer, pageSize, MADV_DONTNEED);
    if (error) {
        throw BufferManagerException("madvise failed with error code " +
                                            to_string(error) +
                                            ": " +
                                            std::strerror(error));
    }
}

void Frame::resetFrameWithoutLock() {
    fileHandlePtr = -1u;
    pageIdx = -1u;
    pinCount = -1u;
    recentlyAccessed = false;
    isDirty = false;
}

bool Frame::acquireFrameLock(bool block) {
    if (block) {
        while (frameLock.test_and_set()) // spinning
            ;
        return true;
    }
    return !frameLock.test_and_set();
}

BufferPool::BufferPool(uint64_t pageSize, uint64_t maxSize)
    : logger{LoggerUtils::getOrCreateLogger("buffer_manager")}, pageSize{pageSize}, clockHand{0},
      numFrames((page_idx_t)(ceil((double)maxSize / (double)pageSize))) {
    assert(pageSize == DEFAULT_PAGE_SIZE || pageSize == LARGE_PAGE_SIZE);
    for (auto i = 0u; i < numFrames; ++i) {
        bufferCache.emplace_back(make_unique<Frame>(pageSize));
    }
    logger->info("Initializing Buffer Pool.");
    logger->info("BufferPool Size {}B, #{}byte-pages {}.", maxSize, pageSize,
        ceil((double)maxSize / (double)pageSize));
    logger->info("Done Initializing Buffer Pool.");
}

void BufferPool::resize(uint64_t newSize) {
    if ((numFrames * pageSize) > newSize) {
        throw BufferManagerException("Resizing to a smaller Buffer Pool Size is unsupported!");
    }
    auto newNumFrames = (page_idx_t)(ceil((double)newSize / (double)pageSize));
    assert(newNumFrames < UINT32_MAX);
    for (auto i = 0u; i < newNumFrames - numFrames; ++i) {
        bufferCache.emplace_back(make_unique<Frame>(pageSize));
    }
    numFrames = newNumFrames;
    logger->info("Resizing buffer pool.");
    logger->info("New buffer pool size {}B, #{}byte-pages {}.", newSize, pageSize, newNumFrames);
    logger->info("Done resizing buffer pool.");
}

uint8_t* BufferPool::pin(FileHandle& fileHandle, page_idx_t pageIdx) {
    return pin(fileHandle, pageIdx, false /* read page from file */);
}

uint8_t* BufferPool::pinWithoutReadingFromFile(FileHandle& fileHandle, page_idx_t pageIdx) {
    return pin(fileHandle, pageIdx, true /* do not read page from file */);
}

void BufferPool::removeFilePagesFromFrames(FileHandle& fileHandle) {
    for (auto pageIdx = 0u; pageIdx < fileHandle.numPages; ++pageIdx) {
        removePageFromFrame(fileHandle, pageIdx, false /* do not flush */);
    }
}

void BufferPool::removePageFromFrame(FileHandle& fileHandle, page_idx_t pageIdx, bool shouldFlush) {
    fileHandle.acquirePageLock(pageIdx, true /*block*/);
    auto frameIdx = fileHandle.getFrameIdx(pageIdx);
    if (FileHandle::isAFrame(frameIdx)) {
        auto& frame = bufferCache[frameIdx];
        frame->acquireFrameLock(true /* block */);
        if (shouldFlush) {
            flushIfDirty(frame);
        }
        clearFrameAndUnswizzleWithoutLock(frame, fileHandle, pageIdx);
        frame->releaseFrameLock();
    }
    fileHandle.releasePageLock(pageIdx);
}

void BufferPool::removePageFromFrameWithoutFlushingIfNecessary(
    FileHandle& fileHandle, page_idx_t pageIdx) {
    if (pageIdx >= fileHandle.numPages) {
        return;
    }
    removePageFromFrame(fileHandle, pageIdx, false /* do not flush */);
}

void BufferPool::flushAllDirtyPagesInFrames(FileHandle& fileHandle) {
    for (auto pageIdx = 0u; pageIdx < fileHandle.numPages; ++pageIdx) {
        removePageFromFrame(fileHandle, pageIdx, true /* flush */);
    }
}

void BufferPool::updateFrameIfPageIsInFrameWithoutPageOrFrameLock(
    FileHandle& fileHandle, uint8_t* newPage, page_idx_t pageIdx) {
    auto frameIdx = fileHandle.getFrameIdx(pageIdx);
    if (FileHandle::isAFrame(frameIdx)) {
        memcpy(bufferCache[frameIdx]->buffer.get(), newPage, DEFAULT_PAGE_SIZE);
    }
}

uint8_t* BufferPool::pin(FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    fileHandle.acquirePageLock(pageIdx, true /*block*/);
    auto retVal = pinWithoutAcquiringPageLock(fileHandle, pageIdx, doNotReadFromFile);
    fileHandle.releasePageLock(pageIdx);
    return retVal;
}

uint8_t* BufferPool::pinWithoutAcquiringPageLock(
    FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    auto frameIdx = fileHandle.getFrameIdx(pageIdx);
    if (FileHandle::isAFrame(frameIdx)) {
        auto& frame = bufferCache[frameIdx];
        frame->pinCount.fetch_add(1);
        frame->recentlyAccessed = true;
        //        bmMetrics.numCacheHit += 1;
    } else {
        frameIdx = claimAFrame(fileHandle, pageIdx, doNotReadFromFile);
        fileHandle.swizzle(pageIdx, frameIdx);
        if (!doNotReadFromFile) {
            bmMetrics.numCacheMiss += 1;
        }
    }
    bmMetrics.numPins += 1;
    return bufferCache[fileHandle.getFrameIdx(pageIdx)]->buffer.get();
}

void BufferPool::setPinnedPageDirty(FileHandle& fileHandle, page_idx_t pageIdx) {
    fileHandle.acquirePageLock(pageIdx, true /*block*/);
    auto frameIdx = fileHandle.getFrameIdx(pageIdx);
    if (!FileHandle::isAFrame((frameIdx)) || (bufferCache[frameIdx]->pinCount.load() < 1)) {
        fileHandle.releasePageLock(pageIdx);
        throw BufferManagerException("If a page is not in memory or is not pinned, cannot set "
                                     "it to isDirty = true.filePath: " +
                                     fileHandle.fileInfo->path + " pageIdx: " + to_string(pageIdx) +
                                     ".");
    }
    bufferCache[frameIdx]->setIsDirty(true /* isDirty */);
    fileHandle.releasePageLock(pageIdx);
}

page_idx_t BufferPool::claimAFrame(
    FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    auto localClockHand = clockHand.load();
    auto startFrame = localClockHand % numFrames;
    for (auto i = 0u; i < 2 * numFrames; ++i) {
        auto frameIdx = (startFrame + i) % numFrames;
        auto pinCount = bufferCache[frameIdx]->pinCount.load();
        if (-1u == pinCount && fillEmptyFrame(frameIdx, fileHandle, pageIdx, doNotReadFromFile)) {
            moveClockHand(localClockHand + i + 1);
            return frameIdx;
        } else if (0u == pinCount && tryEvict(frameIdx, fileHandle, pageIdx, doNotReadFromFile)) {
            moveClockHand(localClockHand + i + 1);
            return frameIdx;
        }
    }
    throw BufferManagerException("Cannot find a frame to evict from.");
}

bool BufferPool::fillEmptyFrame(
    page_idx_t frameIdx, FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    auto& frame = bufferCache[frameIdx];
    if (!frame->acquireFrameLock(false)) {
        return false;
    }
    if (-1u == frame->pinCount.load()) {
        readNewPageIntoFrame(*frame, fileHandle, pageIdx, doNotReadFromFile);
        frame->releaseFrameLock();
        return true;
    }
    frame->releaseFrameLock();
    return false;
}

bool BufferPool::tryEvict(
    page_idx_t frameIdx, FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    auto& frame = bufferCache[frameIdx];
    if (frame->recentlyAccessed) {
        frame->recentlyAccessed = false;
        bmMetrics.numRecentlyAccessedWalkover += 1;
        return false;
    }
    if (!frame->acquireFrameLock(false)) {
        return false;
    }
    auto pageIdxInFrame = frame->pageIdx.load();
    auto fileHandleInFrame = reinterpret_cast<FileHandle*>(frame->fileHandlePtr.load());
    if (!fileHandleInFrame->acquirePageLock(pageIdxInFrame, false)) {
        bmMetrics.numEvictFails += 1;
        frame->releaseFrameLock();
        return false;
    }
    // We check pinCount again after acquiring the lock on page currently residing in the frame. At
    // this point in time, no other thread can change the pinCount.
    if (0u != frame->pinCount.load()) {
        bmMetrics.numEvictFails += 1;
        fileHandleInFrame->releasePageLock(pageIdxInFrame);
        frame->releaseFrameLock();
        return false;
    }
    // Else, flush out the frame into the file page if the frame is dirty. Then remove the page from
    // the frame and release the lock on it.
    flushIfDirty(frame);
    clearFrameAndUnswizzleWithoutLock(frame, *fileHandleInFrame, pageIdxInFrame);
    fileHandleInFrame->releasePageLock(pageIdxInFrame);
    // Update the frame information and release the lock on frame.
    readNewPageIntoFrame(*frame, fileHandle, pageIdx, doNotReadFromFile);
    frame->releaseFrameLock();
    bmMetrics.numEvicts += 1;
    return true;
}

void BufferPool::flushIfDirty(const unique_ptr<Frame>& frame) {
    auto fileHandleInFrame = reinterpret_cast<FileHandle*>(frame->fileHandlePtr.load());
    auto pageIdxInFrame = frame->pageIdx.load();
    if (frame->isDirty) {
        bmMetrics.numDirtyPageWriteIO += 1;
        fileHandleInFrame->writePage(frame->buffer.get(), pageIdxInFrame);
    }
}

void BufferPool::clearFrameAndUnswizzleWithoutLock(
    const unique_ptr<Frame>& frame, FileHandle& fileHandleInFrame, page_idx_t pageIdxInFrame) {
    frame->resetFrameWithoutLock();
    fileHandleInFrame.unswizzle(pageIdxInFrame);
}

void BufferPool::readNewPageIntoFrame(
    Frame& frame, FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    frame.pinCount.store(1);
    frame.recentlyAccessed = true;
    frame.isDirty = false;
    frame.pageIdx.store(pageIdx);
    frame.fileHandlePtr.store(reinterpret_cast<uint64_t>(&fileHandle));
    if (!doNotReadFromFile) {
        fileHandle.readPage(frame.buffer.get(), pageIdx);
    }
}

void BufferPool::moveClockHand(uint64_t newClockHand) {
    do {
        auto currClockHand = clockHand.load();
        if (currClockHand > newClockHand) {
            return;
        }
        if (clockHand.compare_exchange_strong(currClockHand, newClockHand, memory_order_seq_cst)) {
            return;
        }
    } while (true);
}

void BufferPool::unpin(FileHandle& fileHandle, page_idx_t pageIdx) {
    fileHandle.acquirePageLock(pageIdx, true /*block*/);
    unpinWithoutAcquiringPageLock(fileHandle, pageIdx);
    fileHandle.releasePageLock(pageIdx);
}

void BufferPool::unpinWithoutAcquiringPageLock(FileHandle& fileHandle, page_idx_t pageIdx) {
    auto& frame = bufferCache[fileHandle.getFrameIdx(pageIdx)];
    // `count` is the value of `pinCount` before sub.
    auto count = frame->pinCount.fetch_sub(1);
    assert(count >= 1);
}

/*************************/
// --- BufferPoolMmap --- //
/*************************/

BufferPoolMmap::BufferPoolMmap(uint64_t maxSize)
        : logger{LoggerUtils::getOrCreateLogger("buffer_manager")},
          numDefaultFrames((page_idx_t)(ceil((double)maxSize / (double)DEFAULT_PAGE_SIZE))),
          numLargeFrames((page_idx_t)(ceil((double)maxSize / (double)LARGE_PAGE_SIZE))),
          evictionQueue(make_unique<moodycamel::ConcurrentQueue<EvictionQueueNode>>()),
          currentMemory(0), maxMemory(maxSize), frameIdxStart(0) {
    // Call mmap to create two virtual memory regions for each of the two size classes.
    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    uint8_t *default_mmap = (uint8_t *) mmap(NULL, maxSize, prot, flags, -1, 0);
    uint8_t *large_mmap = (uint8_t *) mmap(NULL, maxSize, prot, flags, -1, 0);

    uint8_t* buffer;

    // Create default-sized frames.
    for (auto i = 0u; i < numDefaultFrames; i++) {
        buffer = default_mmap + (i * DEFAULT_PAGE_SIZE);
        defaultPageBufferCache.emplace_back(make_unique<Frame>(DEFAULT_PAGE_SIZE, buffer));
    }

    // Create large-sized frames.
    for (auto i = 0u; i < numLargeFrames; i++) {
        buffer = large_mmap + (i * LARGE_PAGE_SIZE);
        largePageBufferCache.emplace_back(make_unique<Frame>(LARGE_PAGE_SIZE, buffer));
    }

    logger->info("Initializing mmap-based Buffer Pool.");
    logger->info("UmbraBufferPool Size {}B, #{}byte-pages {} #{}byte-pages {}.", maxSize,
                 DEFAULT_PAGE_SIZE, ceil((double)maxSize / (double)DEFAULT_PAGE_SIZE),
                 LARGE_PAGE_SIZE, ceil((double)maxSize / (double)LARGE_PAGE_SIZE));
    logger->info("Done Initializing Buffer Pool.");
}

// Pass pageIdx = -1u to prevent a new page from being read into this frame
bool BufferPoolMmap::tryEvict(
        page_idx_t frameIdx, FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    vector<unique_ptr<Frame>> &bufferCache = fileHandle.isLargePaged() ? largePageBufferCache
                                                                       : defaultPageBufferCache;
    size_t extraMemory = fileHandle.isLargePaged() ? LARGE_PAGE_SIZE : DEFAULT_PAGE_SIZE;

    auto& frame = bufferCache[frameIdx];

// Checking recentlyAccessed for eviction queues will cause evictions to never happen.
//    if (frame->recentlyAccessed) {
//        frame->recentlyAccessed = false;
//        bmMetrics.numRecentlyAccessedWalkover += 1;
//        return false;
//    }

    if (!frame->acquireFrameLock(false)) {
        return false;
    }
    auto pageIdxInFrame = frame->pageIdx.load();
    auto fileHandleInFrame = reinterpret_cast<FileHandle*>(frame->fileHandlePtr.load());
    if (!fileHandleInFrame->acquirePageLock(pageIdxInFrame, false)) {
        bmMetrics.numEvictFails += 1;
        frame->releaseFrameLock();
        return false;
    }
    // We check pinCount again after acquiring the lock on page currently residing in the frame. At
    // this point in time, no other thread can change the pinCount.
    if (0u != frame->pinCount.load()) {
        bmMetrics.numEvictFails += 1;
        fileHandleInFrame->releasePageLock(pageIdxInFrame);
        frame->releaseFrameLock();
        return false;
    }
    // Else, flush out the frame into the file page if the frame is dirty. Then remove the page from
    // the frame and release the lock on it.
    flushIfDirty(frame);
    clearFrameAndUnswizzleWithoutLock(frame, *fileHandleInFrame, pageIdxInFrame);
    fileHandleInFrame->releasePageLock(pageIdxInFrame);
    // Update the frame information and release the lock on frame.
    if (pageIdx != -1u) {
        readNewPageIntoFrame(*frame, fileHandle, pageIdx, doNotReadFromFile);
    } else {
        // Frame is not being reused, decrease currentMemory and call releaseMemory
        // to free physical memory occupied by the frame (using madvise).
        currentMemory -= extraMemory;
        frame->releaseMemory();
    }
    frame->releaseFrameLock();
    bmMetrics.numEvicts += 1;
    return true;
}

void BufferPoolMmap::flushIfDirty(const unique_ptr<Frame>& frame) {
    auto fileHandleInFrame = reinterpret_cast<FileHandle*>(frame->fileHandlePtr.load());
    auto pageIdxInFrame = frame->pageIdx.load();
    if (frame->isDirty) {
        bmMetrics.numDirtyPageWriteIO += 1;
        fileHandleInFrame->writePage(frame->mmapBuffer, pageIdxInFrame);
    }
}

void BufferPoolMmap::clearFrameAndUnswizzleWithoutLock(
        const unique_ptr<Frame>& frame, FileHandle& fileHandleInFrame, page_idx_t pageIdxInFrame) {
    frame->resetFrameWithoutLock();
    fileHandleInFrame.unswizzle(pageIdxInFrame);
}

void BufferPoolMmap::readNewPageIntoFrame(
        Frame& frame, FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    frame.pinCount.store(1);
    frame.recentlyAccessed = true;
    frame.isDirty = false;
    frame.pageIdx.store(pageIdx);
    frame.fileHandlePtr.store(reinterpret_cast<uint64_t>(&fileHandle));
    if (!doNotReadFromFile) {
        fileHandle.readPage(frame.mmapBuffer, pageIdx);
    }
}

uint8_t* BufferPoolMmap::pin(FileHandle& fileHandle, page_idx_t pageIdx) {
    return pin(fileHandle, pageIdx, false /* read page from file */);
}

uint8_t* BufferPoolMmap::pinWithoutReadingFromFile(FileHandle& fileHandle, page_idx_t pageIdx) {
    return pin(fileHandle, pageIdx, true /* do not read page from file */);
}

void BufferPoolMmap::removeFilePagesFromFrames(FileHandle& fileHandle) {
    for (auto pageIdx = 0u; pageIdx < fileHandle.numPages; ++pageIdx) {
        removePageFromFrame(fileHandle, pageIdx, false /* do not flush */);
    }
}

void BufferPoolMmap::removePageFromFrame(FileHandle& fileHandle, page_idx_t pageIdx, bool shouldFlush) {
    fileHandle.acquirePageLock(pageIdx, true /*block*/);
    auto frameIdx = fileHandle.getFrameIdx(pageIdx);
    vector<unique_ptr<Frame>> &bufferCache = fileHandle.isLargePaged() ? largePageBufferCache
                                                                       : defaultPageBufferCache;
    if (FileHandle::isAFrame(frameIdx)) {
        auto& frame = bufferCache[frameIdx];
        frame->acquireFrameLock(true /* block */);
        if (shouldFlush) {
            flushIfDirty(frame);
        }
        clearFrameAndUnswizzleWithoutLock(frame, fileHandle, pageIdx);
        frame->releaseFrameLock();
    }
    fileHandle.releasePageLock(pageIdx);
}

void BufferPoolMmap::removePageFromFrameWithoutFlushingIfNecessary(
        FileHandle& fileHandle, page_idx_t pageIdx) {
    if (pageIdx >= fileHandle.numPages) {
        return;
    }
    removePageFromFrame(fileHandle, pageIdx, false /* do not flush */);
}

void BufferPoolMmap::flushAllDirtyPagesInFrames(FileHandle& fileHandle) {
    for (auto pageIdx = 0u; pageIdx < fileHandle.numPages; ++pageIdx) {
        removePageFromFrame(fileHandle, pageIdx, true /* flush */);
    }
}

void BufferPoolMmap::updateFrameIfPageIsInFrameWithoutPageOrFrameLock(
        FileHandle& fileHandle, uint8_t* newPage, page_idx_t pageIdx) {
    auto frameIdx = fileHandle.getFrameIdx(pageIdx);
    vector<unique_ptr<Frame>> &bufferCache = fileHandle.isLargePaged() ? largePageBufferCache
                                                                       : defaultPageBufferCache;
    if (FileHandle::isAFrame(frameIdx)) {
        memcpy(bufferCache[frameIdx]->mmapBuffer, newPage, DEFAULT_PAGE_SIZE);
    }
}

uint8_t* BufferPoolMmap::pin(FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    fileHandle.acquirePageLock(pageIdx, true /*block*/);
    auto retVal = pinWithoutAcquiringPageLock(fileHandle, pageIdx, doNotReadFromFile);
    fileHandle.releasePageLock(pageIdx);
    return retVal;
}

uint8_t* BufferPoolMmap::pinWithoutAcquiringPageLock(
        FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    vector<unique_ptr<Frame>> &bufferCache = fileHandle.isLargePaged() ? largePageBufferCache
                                                                       : defaultPageBufferCache;
    auto frameIdx = fileHandle.getFrameIdx(pageIdx);
    if (FileHandle::isAFrame(frameIdx)) {
        auto& frame = bufferCache[frameIdx];
        frame->pinCount.fetch_add(1);
        frame->recentlyAccessed = true;
        //        bmMetrics.numCacheHit += 1;
    } else {
        frameIdx = claimAFrame(fileHandle, pageIdx, doNotReadFromFile);
        fileHandle.swizzle(pageIdx, frameIdx);
        if (!doNotReadFromFile) {
            bmMetrics.numCacheMiss += 1;
        }
    }
    bmMetrics.numPins += 1;
    return bufferCache[fileHandle.getFrameIdx(pageIdx)]->mmapBuffer;
}

void BufferPoolMmap::setPinnedPageDirty(FileHandle& fileHandle, page_idx_t pageIdx) {
    vector<unique_ptr<Frame>> &bufferCache = fileHandle.isLargePaged() ? largePageBufferCache
                                                                       : defaultPageBufferCache;

    fileHandle.acquirePageLock(pageIdx, true /*block*/);
    auto frameIdx = fileHandle.getFrameIdx(pageIdx);
    if (!FileHandle::isAFrame((frameIdx)) || (bufferCache[frameIdx]->pinCount.load() < 1)) {
        fileHandle.releasePageLock(pageIdx);
        throw BufferManagerException("If a page is not in memory or is not pinned, cannot set "
                                     "it to isDirty = true.filePath: " +
                                     fileHandle.fileInfo->path + " pageIdx: " + to_string(pageIdx) +
                                     ".");
    }
    bufferCache[frameIdx]->setIsDirty(true /* isDirty */);
    fileHandle.releasePageLock(pageIdx);
}


page_idx_t BufferPoolMmap::claimAFrame(FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    vector<unique_ptr<Frame>> &bufferCache = fileHandle.isLargePaged() ? largePageBufferCache
                                                                      : defaultPageBufferCache;
    size_t extraMemory = fileHandle.isLargePaged() ? LARGE_PAGE_SIZE : DEFAULT_PAGE_SIZE;

    purgeQueue();

    // Evict pages until there's enough memory
    EvictionQueueNode victim;
    while(currentMemory + extraMemory > maxMemory) {
        if (!evictionQueue->try_dequeue(victim)) {
            // There aren't frames that can be evicted to make space for extraMemory.
            throw BufferManagerException("Cannot find a frame to evict from.");
        }

        // The frame has been inserted further up the queue, move on
        if (victim.timestamp != victim.frame->eviction_timestamp) {
            continue;
        }

        auto pinCount = victim.frame->pinCount.load();

        // If the victim frame has the same pageSize as the requested frame, reuse it.
        if (0u == pinCount && victim.frame->pageSize == extraMemory) {
            // Pass in requested frame's fileHandle to load page into frame
            if (tryEvict(victim.frameIdx, fileHandle, pageIdx, doNotReadFromFile)) {
                return victim.frameIdx;
            } else {
                // Unable to evict this frame, keep going through the queue
                continue;
            }
        }

        // Different sizes, evict but don't reuse because of size mismatch
        if (0u == pinCount) {
            // pageIdx is -1u since we don't want a new page to be read into this frame
            // Pass in victim frame's fileHandle for correct frame eviction
            tryEvict(victim.frameIdx, *victim.fileHandle, -1u, false);
            continue; // Keep going regardless of whether eviction was successful
        }
    }

    // Go through bufferCache and find frame with pinCount == -1 to fill page with
    auto numFrames = fileHandle.isLargePaged() ? numLargeFrames : numDefaultFrames;
    auto currFrameIdxStart = frameIdxStart.load();

    for (auto frameNum = 0u; frameNum < numFrames; ++frameNum) {
        auto frameIdx = (currFrameIdxStart + frameNum) % numFrames;
        auto pinCount = bufferCache[frameIdx]->pinCount.load();

        if (-1u == pinCount && fillEmptyFrame(frameIdx, fileHandle, pageIdx, doNotReadFromFile)) {
            frameIdxStart.store(frameIdx);
            return frameIdx;
        }
    }

    throw BufferManagerException("Cannot find a frame to claim.");
}

bool BufferPoolMmap::fillEmptyFrame(
        page_idx_t frameIdx, FileHandle& fileHandle, page_idx_t pageIdx, bool doNotReadFromFile) {
    vector<unique_ptr<Frame>> &bufferCache = fileHandle.isLargePaged() ? largePageBufferCache
                                                                       : defaultPageBufferCache;
    size_t extraMemory = fileHandle.isLargePaged() ? LARGE_PAGE_SIZE : DEFAULT_PAGE_SIZE;
    auto& frame = bufferCache[frameIdx];
    if (!frame->acquireFrameLock(false)) {
        return false;
    }
    if (-1u == frame->pinCount.load()) {
        readNewPageIntoFrame(*frame, fileHandle, pageIdx, doNotReadFromFile);
        currentMemory += extraMemory;
        frame->releaseFrameLock();
        return true;
    }
    frame->releaseFrameLock();
    return false;
}

void BufferPoolMmap::unpin(FileHandle& fileHandle, page_idx_t pageIdx) {
    fileHandle.acquirePageLock(pageIdx, true /*block*/);
    unpinWithoutAcquiringPageLock(fileHandle, pageIdx);
    fileHandle.releasePageLock(pageIdx);
}

void BufferPoolMmap::unpinWithoutAcquiringPageLock(FileHandle& fileHandle, page_idx_t pageIdx) {
    vector<unique_ptr<Frame>> &bufferCache = fileHandle.isLargePaged() ? largePageBufferCache
                                                                       : defaultPageBufferCache;
    page_idx_t frameIdx = fileHandle.getFrameIdx(pageIdx);
    auto& frame = bufferCache[frameIdx];
    // `count` is the value of `pinCount` before sub.
    auto count = frame->pinCount.fetch_sub(1);
    assert(count >= 1);
    if (count == 0) {
        addToEvictionQueue(frame.get(), frameIdx, &fileHandle);
    }
}

void BufferPoolMmap::addToEvictionQueue(Frame *frame, page_idx_t frameIdx, FileHandle *fileHandle) {
    // This is the latest node that's tracking this frame, so increment eviction_timestamp
    frame->eviction_timestamp++;
    purgeQueue();
    // enqueue uses move semantics
    evictionQueue->enqueue(EvictionQueueNode(frame, frameIdx, fileHandle, frame->eviction_timestamp));
}

void BufferPoolMmap::purgeQueue() {
    EvictionQueueNode node;
    while (true) {
        if (!evictionQueue->try_dequeue(node)) {
            break;
        }
        auto pinCount = node.frame->pinCount.load();
        if (pinCount == 0 && node.timestamp == node.frame->eviction_timestamp) {
            // pinCount is still 0 && this node is the latest node keeping track of this frame,
            // enqueue it back for eventual eviction
            evictionQueue->enqueue(node);
            break;
        } else {
            // Otherwise, throw it away
            continue;
        }
    }
}

} // namespace storage
} // namespace kuzu
