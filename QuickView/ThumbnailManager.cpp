#include "pch.h"
#include "ThumbnailManager.h"
#include "ImageEngine.h"
#include "ImageLoaderSimd.h"
#include <algorithm>
#include <cwctype>
#include "FileNavigator.h"
extern FileNavigator& g_navigator;
extern ImageEngine* g_pImageEngine;

namespace {
unsigned GetPhysicalProcessorCount() {
    DWORD byteCount = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr,
                                         &byteCount) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || byteCount == 0) {
        return (std::max)(1u, std::thread::hardware_concurrency() / 2u);
    }

    std::vector<unsigned char> buffer(byteCount);
    auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
        buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info,
                                          &byteCount)) {
        return (std::max)(1u, std::thread::hardware_concurrency() / 2u);
    }

    unsigned coreCount = 0;
    const auto* end = buffer.data() + byteCount;
    auto* cursor = buffer.data();
    while (cursor < end) {
        const auto* entry = reinterpret_cast<
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(cursor);
        if (entry->Relationship == RelationProcessorCore) ++coreCount;
        if (entry->Size == 0) break;
        cursor += entry->Size;
    }
    return (std::max)(1u, coreCount);
}
bool IsThumbnailAdequate(const CImageLoader::ThumbData& data,
                         int requestedSize) {
    const int decodedCropAxis = (std::min)(data.width, data.height);
    if (decodedCropAxis >= requestedSize) return true;
    const int intrinsicCropAxis =
        (std::min)(data.origWidth, data.origHeight);
    return intrinsicCropAxis > 0 && decodedCropAxis >= intrinsicCropAxis;
}

bool TryLoadCachedFrame(const std::wstring& path, int targetSize,
                        CImageLoader::ThumbData& out) {
    if (!g_pImageEngine || targetSize <= 0) return false;

    CImageLoader::ImageMetadata metadata;
    const auto frame = g_pImageEngine->GetCachedImage(path, &metadata);
    if (!frame || !frame->IsValid() || !frame->pixels ||
        frame->width <= 0 || frame->height <= 0 ||
        frame->stride < frame->width * 4 ||
        metadata.ExifOrientation != 1 ||
        (frame->format != QuickView::PixelFormat::BGRA8888 &&
         frame->format != QuickView::PixelFormat::BGRX8888)) {
        return false;
    }

    const int cropAxis = (std::min)(frame->width, frame->height);
    if (cropAxis < targetSize) return false;
    const float scale = static_cast<float>(targetSize) /
                        static_cast<float>(cropAxis);
    const int dstW =
        (std::max)(1, static_cast<int>(std::lround(frame->width * scale)));
    const int dstH =
        (std::max)(1, static_cast<int>(std::lround(frame->height * scale)));
    const int dstStride = dstW * 4;
    out.pixels.resize(static_cast<size_t>(dstStride) * dstH);

    if (dstW == frame->width && dstH == frame->height) {
        for (int y = 0; y < dstH; ++y) {
            memcpy(out.pixels.data() + static_cast<size_t>(y) * dstStride,
                   frame->pixels + static_cast<size_t>(y) * frame->stride,
                   dstStride);
        }
    } else {
        ImageLoaderSimd::ResizeBilinear(
            frame->pixels, frame->width, frame->height, frame->stride,
            out.pixels.data(), dstW, dstH, dstStride);
    }

    out.width = dstW;
    out.height = dstH;
    out.stride = dstStride;
    out.origWidth = metadata.Width > 0 ? static_cast<int>(metadata.Width)
                                       : frame->width;
    out.origHeight = metadata.Height > 0 ? static_cast<int>(metadata.Height)
                                         : frame->height;
    out.fileSize = metadata.FileSize;
    out.isValid = true;
    out.isBlurry = false;
    out.loaderName = L"ImageEngine Cache";
    return true;
}
} // namespace

ThumbnailManager::ThumbnailManager() {}

ThumbnailManager::~ThumbnailManager() {
    Shutdown();
}

void ThumbnailManager::Initialize(HWND hwnd, CImageLoader* pLoader) {
    m_hwnd = hwnd;
    m_pLoader = pLoader;
    m_running = true;

    // One independent thumbnail job per physical core. AVIF target previews
    // are single-threaded, so this is a hard 16-core decode budget.
    const unsigned fastWorkerCount =
        (std::min)(16u, GetPhysicalProcessorCount());
    m_workerThreadsFast.reserve(fastWorkerCount);
    for (unsigned i = 0; i < fastWorkerCount; ++i) {
        m_workerThreadsFast.emplace_back(&ThumbnailManager::WorkerLoopFast, this);
    }
    m_workerThreadSlow = std::thread(&ThumbnailManager::WorkerLoopSlow, this);
}

void ThumbnailManager::Shutdown() {
    m_running = false;
    m_cvFast.notify_all();
    m_cvSlow.notify_all();
    for (auto& worker : m_workerThreadsFast) {
        if (worker.joinable()) worker.join();
    }
    m_workerThreadsFast.clear();
    if (m_workerThreadSlow.joinable()) m_workerThreadSlow.join();
    ClearCache();
}

void ThumbnailManager::ClearCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_l1Cache.clear();
    m_l2Cache.clear();
    m_lruList.clear();
    m_lruMap.clear();
    m_currentCacheSize = 0;
    
    // Also clear queue?
    std::lock_guard<std::mutex> queueLock(m_queueMutex);
    m_currentGeneration++; // Invalidate pending work
    m_pendingTasks.clear();
    m_fastQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_slowQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_priorityStart = -1;
    m_priorityEnd = -1;
    m_priorityCenter = -1;
}

ComPtr<ID2D1Bitmap> ThumbnailManager::GetThumbnail(
    size_t imageId, LPCWSTR /*filePath*/, ID2D1RenderTarget* pRT,
    int targetSize, bool* needsRequest) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (needsRequest) *needsRequest = false;
    const int requestedSize = std::clamp(targetSize, 64, 1024);

    auto itL2 = m_l2Cache.find(imageId);
    if (itL2 != m_l2Cache.end()) {
        TouchLRU(imageId);
        const D2D1_SIZE_F size = itL2->second->GetSize();
        if (needsRequest) {
            bool adequate =
                (std::min)(size.width, size.height) + 0.5f >=
                static_cast<float>(requestedSize);
            if (!adequate) {
                if (auto raw = m_l1Cache.find(imageId);
                    raw != m_l1Cache.end()) {
                    adequate = IsThumbnailAdequate(
                        raw->second, requestedSize);
                }
            }
            *needsRequest = !adequate;
        }
        return itL2->second;
    }

    auto itL1 = m_l1Cache.find(imageId);
    if (itL1 != m_l1Cache.end()) {
        TouchLRU(imageId);
        if (itL1->second.isFailed) return nullptr;

        if (needsRequest) {
            *needsRequest =
                !IsThumbnailAdequate(itL1->second, requestedSize);
        }

        ComPtr<ID2D1Bitmap> bmp;
        if (pRT && !itL1->second.pixels.empty()) {
            const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED));
            const D2D1_SIZE_U size =
                D2D1::SizeU(itL1->second.width, itL1->second.height);
            const HRESULT hr =
                pRT->CreateBitmap(size, itL1->second.pixels.data(),
                                  itL1->second.stride, &props, &bmp);
            if (SUCCEEDED(hr)) {
                m_l2Cache[imageId] = bmp;
            } else {
                itL1->second.isFailed = true;
                itL1->second.pixels.clear();
                itL1->second.pixels.shrink_to_fit();
                if (needsRequest) *needsRequest = false;
            }
        }
        return bmp;
    }

    if (needsRequest) *needsRequest = true;
    return nullptr;
}

void ThumbnailManager::UpdateOptimizedPriority(int startIdx, int endIdx,
                                               int priorityCenter) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (startIdx == m_priorityStart && endIdx == m_priorityEnd &&
        priorityCenter == m_priorityCenter) {
        return;
    }

    m_priorityStart = startIdx;
    m_priorityEnd = endIdx;
    m_priorityCenter = priorityCenter;

    auto reprioritize = [&](auto& queue) {
        using Queue = std::decay_t<decltype(queue)>;
        Queue updated;
        while (!queue.empty()) {
            Task task = queue.top();
            queue.pop();
            if (task.itemIndex < startIdx || task.itemIndex > endIdx) {
                if (auto pending = m_pendingTasks.find(task.imageId);
                    pending != m_pendingTasks.end() &&
                    pending->second == task.generation) {
                    m_pendingTasks.erase(pending);
                }
                continue;
            }
            task.priorityDistance = std::abs(task.itemIndex - priorityCenter);
            updated.push(std::move(task));
        }
        queue = std::move(updated);
    };

    // A scrollbar jump must not wait behind thumbnails from the old viewport.
    // In-flight work is allowed to finish and remains cached.
    reprioritize(m_fastQueue);
    reprioritize(m_slowQueue);
}

// Helper (Internal or Public?) - Let's make it Public for Overlay to use iteratively
// Actually, let's change UpdateOptimizedPriority to accept a list of needed thumbs?
// Or just let Overlay loop and call "QueueIfNotCached".
// Let's add `QueueRequest(int index, LPCWSTR path, int priority)` to public API.
// And `ClearQueue()` for the "Fast Scroll" cancellation.

void ThumbnailManager::EvictLRU() {
    // Must be called with m_cacheMutex locked
    while (!m_lruList.empty() && (m_currentCacheSize > MAX_CACHE_SIZE || m_lruMap.size() > MAX_CACHE_COUNT)) {
        size_t idxToRemove = m_lruList.back();
        
        // [Fix] Correctly track memory reduction before erasing
        auto itL1 = m_l1Cache.find(idxToRemove);
        if (itL1 != m_l1Cache.end()) {
            size_t size = itL1->second.pixels.size();
            if (m_currentCacheSize >= size) m_currentCacheSize -= size;
            else m_currentCacheSize = 0;
            m_l1Cache.erase(itL1);
        }
        
        m_l2Cache.erase(idxToRemove);
        m_lruMap.erase(idxToRemove);
        m_lruList.pop_back();
    }
}

void ThumbnailManager::AddToLRU(size_t imageId, size_t size) {
    auto it = m_lruMap.find(imageId);
    if (it != m_lruMap.end()) {
        // [Fix] If exists, just move to front. 
        // We assume size doesn't change significantly for the same ID once in L1.
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
        return;
    }
    
    m_currentCacheSize += size;
    m_lruList.push_front(imageId);
    m_lruMap[imageId] = m_lruList.begin();
    
    EvictLRU();
}

void ThumbnailManager::TouchLRU(size_t imageId) {
    auto it = m_lruMap.find(imageId);
    if (it != m_lruMap.end()) {
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
    }
}
bool ThumbnailManager::StoreDecodedThumbnail(
    size_t imageId, CImageLoader::ThumbData&& data) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    if (auto existing = m_l1Cache.find(imageId);
        existing != m_l1Cache.end() && !existing->second.isFailed &&
        (std::min)(existing->second.width, existing->second.height) >=
            (std::min)(data.width, data.height)) {
        return false;
    }
    if (auto existing = m_l1Cache.find(imageId);
        existing != m_l1Cache.end()) {
        const size_t oldSize = existing->second.pixels.size();
        m_currentCacheSize =
            oldSize <= m_currentCacheSize ? m_currentCacheSize - oldSize : 0;
    }
    m_l2Cache.erase(imageId);
    if (auto lru = m_lruMap.find(imageId); lru != m_lruMap.end()) {
        m_lruList.erase(lru->second);
        m_lruMap.erase(lru);
    }

    const size_t size = data.pixels.size();
    m_l1Cache[imageId] = std::move(data);
    AddToLRU(imageId, size);
    return true;
}

void ThumbnailManager::FinishPendingTask(const Task& task) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (auto pending = m_pendingTasks.find(task.imageId);
        pending != m_pendingTasks.end() &&
        pending->second == task.generation) {
        m_pendingTasks.erase(pending);
    }
}

void ThumbnailManager::WorkerLoopFast() {
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (m_running) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cvFast.wait(lock, [this] {
                return !m_fastQueue.empty() || !m_running;
            });
            if (!m_running) break;
            if (m_fastQueue.empty()) continue;
            task = m_fastQueue.top();
            m_fastQueue.pop();
        }

        if (task.generation != m_currentGeneration) {
            FinishPendingTask(task);
            continue;
        }

        const int targetSize = task.targetSize;
        CImageLoader::ThumbData data;
        const bool servedFromImageCache =
            TryLoadCachedFrame(task.path, targetSize, data);
        const HRESULT hr = servedFromImageCache
                               ? S_OK
                               : m_pLoader->LoadThumbnail(
                                     task.path.c_str(), targetSize, &data);
        if (FAILED(hr) || !data.isValid) {
            data.isValid = true;
            data.isFailed = true;
            data.width = 1;
            data.height = 1;
            data.stride = 4;
            data.pixels = {0x80, 0x80, 0x80, 0xFF};
            data.loaderName = L"Failure Placeholder";
        }
        bool stored = false;
        if (data.isValid && task.generation == m_currentGeneration) {
            stored = StoreDecodedThumbnail(task.imageId, std::move(data));
        }
        // Clear pending before waking the UI. A higher-size request made by
        // that paint must be admitted rather than lost behind this task.
        FinishPendingTask(task);
        if (stored) {
            PostMessage(m_hwnd, WM_THUMB_KEY_READY, (WPARAM)task.imageId, 0);
        }
        FinishPendingTask(task);
    }
    if (SUCCEEDED(coInitHr)) CoUninitialize();
}

void ThumbnailManager::WorkerLoopSlow() {
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (m_running) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cvSlow.wait(lock, [this] {
                return !m_slowQueue.empty() || !m_running;
            });
            if (!m_running) break;
            if (m_slowQueue.empty()) continue;
            task = m_slowQueue.top();
            m_slowQueue.pop();
        }

        if (task.generation != m_currentGeneration) {
            FinishPendingTask(task);
            continue;
        }

        const int targetSize = task.targetSize;
        CImageLoader::ThumbData data;
        const bool servedFromImageCache =
            TryLoadCachedFrame(task.path, targetSize, data);
        const HRESULT hr = servedFromImageCache
                               ? S_OK
                               : m_pLoader->LoadThumbnail(
                                     task.path.c_str(), targetSize, &data, true);
        if (FAILED(hr) || !data.isValid) {
            data.isValid = true;
            data.isFailed = true;
            data.width = 1;
            data.height = 1;
            data.stride = 4;
            data.pixels = {0x80, 0x80, 0x80, 0xFF};
            data.loaderName = L"Failure Placeholder (Archive)";
        }
        bool stored = false;
        if (data.isValid && task.generation == m_currentGeneration) {
            stored = StoreDecodedThumbnail(task.imageId, std::move(data));
        }
        FinishPendingTask(task);
        if (stored) {
            PostMessage(m_hwnd, WM_THUMB_KEY_READY, (WPARAM)task.imageId, 0);
        }
    }
    if (SUCCEEDED(coInitHr)) CoUninitialize();
}

void ThumbnailManager::QueueRequest(size_t imageId, LPCWSTR path,
                                    int itemIndex, int targetSize) {
    const int requestedSize = std::clamp(targetSize, 64, 1024);
    {
        std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
        if (auto raw = m_l1Cache.find(imageId); raw != m_l1Cache.end()) {
            if (raw->second.isFailed ||
                IsThumbnailAdequate(raw->second, requestedSize)) {
                return;
            }
        }
        if (auto bitmap = m_l2Cache.find(imageId);
            bitmap != m_l2Cache.end()) {
            const D2D1_SIZE_F size = bitmap->second->GetSize();
            bool adequate =
                (std::min)(size.width, size.height) + 0.5f >=
                static_cast<float>(requestedSize);
            if (!adequate) {
                if (auto raw = m_l1Cache.find(imageId);
                    raw != m_l1Cache.end()) {
                    adequate = IsThumbnailAdequate(
                        raw->second, requestedSize);
                }
            }
            if (adequate) return;
        }
    }

    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    if (m_pendingTasks.count(imageId)) return; // Already queued
    
    Task t;
    t.imageId = imageId;
    t.path = path;
    t.itemIndex = itemIndex;
    t.priorityDistance =
        m_priorityCenter >= 0 ? std::abs(itemIndex - m_priorityCenter) : 0;
    t.targetSize = requestedSize;
    t.generation = m_currentGeneration;

    // Detect if this is a virtual archive path
    std::wstring archivePath;
    size_t archiveIndex = 0;
    if (FileNavigator::ParseVirtualPath(path, archivePath, archiveIndex)) {
        t.isArchive = true;
        t.archiveIndex = (int)archiveIndex;
        t.archivePathHash = ComputePathHash(archivePath);
    }

    // Detect format for Lane Selection
    // Fast Lane: JPEG (Optimized) + RAW/HEIC/PSD (Embedded Preview) + WebP (Scaled)
    const std::wstring_view e = QuickView::ExtensionOf(path);
    const bool isFast =
        QuickView::ExtEqualsIgnoreCase(e, L".jpg") || QuickView::ExtEqualsIgnoreCase(e, L".jpeg") ||
        QuickView::ExtEqualsIgnoreCase(e, L".jpe") || QuickView::ExtEqualsIgnoreCase(e, L".jfif") ||
        QuickView::IsRawExtension(e) || QuickView::IsHeifExtension(e) ||
        QuickView::ExtEqualsIgnoreCase(e, L".avif") ||
        QuickView::ExtEqualsIgnoreCase(e, L".psd") || QuickView::ExtEqualsIgnoreCase(e, L".psb") ||
        QuickView::ExtEqualsIgnoreCase(e, L".webp");
    t.isFastLane = isFast;

    if (isFast) {
        m_fastQueue.push(t);
        m_cvFast.notify_one();
    } else {
        m_slowQueue.push(t);
        m_cvSlow.notify_one();
    }
    
    m_pendingTasks[imageId] = t.generation;
}

void ThumbnailManager::ClearQueue() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_currentGeneration++; // Invalidate pending background extractions
    m_fastQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_slowQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_pendingTasks.clear();
    m_priorityStart = -1;
    m_priorityEnd = -1;
    m_priorityCenter = -1;
}




ThumbnailManager::ImageInfo ThumbnailManager::GetImageInfo(size_t imageId) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_l1Cache.find(imageId);
    if (it != m_l1Cache.end()) {
        ImageInfo info;
        info.origWidth = it->second.origWidth;
        info.origHeight = it->second.origHeight;
        info.fileSize = it->second.fileSize;
        info.isValid = true;
        info.isFailed = it->second.isFailed;
        return info;
    }
    return { 0, 0, 0, false, false };
}

