#include "ImageEngine.h"
#include "QuickViewETW.h"
static constexpr const char* CURRENT_MODULE = "ImageEngine";
#include "DebugMetrics.h"
#include "FileNavigator.h"
#include "HeavyLanePool.h"  // [N+1] Include pool implementation
#include "EditState.h"      // [v9.9] Access g_runtime.ForceRawDecode for dispatch decisions
#include "TileManager.h"    // [Infinity Engine]

#include <algorithm>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <cctype>
#include <chrono>
#include <new>

namespace {
    struct AuxLayerReadyCtx {
        ImageEngine* engine;
        std::wstring path;
        ImageID id;
        PaneSlot targetSlot;
        uint64_t generationId;
    };
}

extern HWND g_mainHwnd;

// ============================================================================
// ImageEngine Implementation
// ============================================================================

ImageEngine::ImageEngine(CImageLoader* loader)
    : m_loader(loader)
    , m_fastLane(this, loader)
{
    // [N+1] Detect hardware and create pool
    const SystemInfo& sysInfo = SystemInfo::Cached();
    m_engineConfig = EngineConfig::FromHardware(sysInfo);
    
    // [N+1] Uncapped Pool (User Request)
    // Default is now max(2, CPU - 2) from EngineConfig.
    // We do NOT clamp to 8 or half-CPU anymore.
    // safeLimit logic removed.
    
    // [Unified Architecture] Initialize 3-Arena System
    m_pool.Initialize(ArenaConfig::Detect());
    
    m_heavyPool = std::make_unique<HeavyLanePool>(this, loader, &m_pool, m_engineConfig);
    
    // [Infinity Engine]
    m_tileManager = std::make_shared<QuickView::TileManager>();

    // Debug output
    QV_LOG("Engine_Init",
        TraceLoggingInt32(m_engineConfig.maxHeavyWorkers, "MaxWorkers"));
}

ImageEngine::~ImageEngine() {
    // Stop HeavyLanePool threads before member destruction to prevent use-after-free
    m_heavyPool.reset();
}

void ImageEngine::SetWindow(HWND hwnd) {
    m_hwnd = hwnd;
}

// ============================================================================
// Memory Management Implementations
// ============================================================================

bool QuantumArena::ShouldShrinkMemory() noexcept {
    extern AppConfig g_config;
    if (g_config.MemoryReclaimStrategy == 2) {
        return true; // Always shrink (On-Demand)
    } else if (g_config.MemoryReclaimStrategy == 1) {
        return false; // Never shrink (Aggressive)
    } else {
        // Smart mode: check physical RAM
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        GlobalMemoryStatusEx(&memInfo);
        
        // Return true if available physical memory < 2GB (Tight / Alarm)
        return memInfo.ullAvailPhys < (2ULL * 1024 * 1024 * 1024);
    }
}

void ImageEngine::TryReclaimMemory() {
    if (m_pool.GetTotalCapacity() > 0) {
        // Need to cast away const because GetScoutArenaPtr() returns const
        const QuantumArena* scout = m_pool.GetScoutArenaPtr();
        if (scout) const_cast<QuantumArena*>(scout)->Shrink();
        
        const QuantumArena* heavy0 = m_pool.GetHeavyArena0Ptr();
        if (heavy0) const_cast<QuantumArena*>(heavy0)->Shrink();
        
        // heavy1 is not directly exposed by const ptr?
        // Let's just use the non-const getters if we have them, or GetTotalCapacity implies initialized
        // Wait, m_pool has GetActiveHeavyArena() and GetBackHeavyArena().
        m_pool.GetActiveHeavyArena().Shrink();
        m_pool.GetBackHeavyArena().Shrink();
    }
    
    if (m_heavyPool) {
        m_heavyPool->ShrinkMemory();
    }
}

void QuickView_TryReclaimMemory() {
    extern ImageEngine* g_pImageEngine;
    if (g_pImageEngine) {
        g_pImageEngine->TryReclaimMemory();
    }
}

void ImageEngine::SetHighPriorityMode(bool enabled) {
    m_isHighPriority = enabled;
}

void ImageEngine::QueueEvent(EngineEvent&& e) {
    // Store event in manual queue
    {
        std::lock_guard lock(m_manualQueueMutex);
        m_manualEventQueue.push_back(std::move(e));
    }
    
    // Notify Main Thread
    if (m_hwnd) {
        // WM_ENGINE_EVENT = WM_APP + 3 (Must match main.cpp)
        PostMessageW(m_hwnd, 0x8000 + 3, 0, 0); 
    }
}

// The Main Input: "User wants to go here"
void ImageEngine::UpdateConfig(const RuntimeConfig& cfg) {
    m_config = cfg;
}

// FastLane produced the final frame; cancel only its redundant heavy decode.
// Other lookahead jobs must continue warming the navigation direction.
void ImageEngine::CancelHeavy(ImageID imageId, PaneSlot targetSlot) {
    if (m_heavyPool) {
        m_heavyPool->CancelImage(imageId, targetSlot);
    }
}

void ImageEngine::SetTargetHdrHeadroomStops(float stops) {
    m_targetHdrHeadroomStops.store(stops, std::memory_order_relaxed);
    if (m_heavyPool) {
        m_heavyPool->SetTargetHdrHeadroomStops(stops);
    }
}

// Request full resolution decode for current image (used by JXL serial pipeline)
void ImageEngine::RequestFullDecode(const std::wstring& path, ImageID imageId, PaneSlot targetSlot, uint64_t generationId) {
    // Node B: Decoding Complete / Request Full Decode
    QV_LOG("ImageEngine_FullDecode",
        TraceLoggingInt32(g_debugMetrics.lastUploadChannel.load(), "LastUploadChannel"),
        TraceLoggingInt32(g_debugMetrics.rawFrameUploadCount.load(), "RawUploadCount")
    );

    if (path.empty()) return;
    if (!m_heavyPool) return;
    
    // Only proceed if this is still the current image
    if (imageId != m_currentImageIdBySlot[static_cast<int>(targetSlot)].load()) {
        QV_LOG("Engine_FullDecode", TraceLoggingString("Cancelled ImageChanged", "Action"));
        return;
    }
    
    // [Titan] If MMF is valid, we are in Titan Mode.
    // The Base Layer is already loaded (Scaled). We do NOT want a Full Decode 
    // because it causes OOM/Seconds-long stall and logic issue.
    if (m_mmf && m_mmf->IsValid()) {
        QV_LOG("Engine_FullDecode", TraceLoggingString("Skipped TitanActive", "Action"));
        return;
    }

    // Submit to Heavy Lane with targetWidth=0 to force full resolution decode
    // [Note] No MMF passed here because this is a delayed request (MMF might not persist unless Titan)
    // Actually, if we are in Titan mode, m_mmf is valid. If not, it's null.
    // It's safe to pass m_mmf (member) here.
    m_heavyPool->SubmitFullDecode(path, imageId, m_mmf, targetSlot, generationId);
    
    QV_LOG("Engine_FullDecode",
        TraceLoggingString("Requested", "Action"),
        TraceLoggingUInt64((uint64_t)imageId, "ImageID"));
}

// [Phase 2] Dispatcher Implementation
void ImageEngine::DispatchImageLoad(const std::wstring& path, ImageID imageId, uintmax_t fileSize, PaneSlot targetSlot, uint64_t generationId) {
    // Hot navigation must not touch disk. Prefetched normal frames already
    // carry complete metadata and can be published directly from the cache.
    if (m_forceRefresh.exchange(false)) {
        InvalidateCache(path);
    } else {
        CImageLoader::ImageMetadata cachedMetadata;
        auto cachedFrame = GetCachedImage(path, &cachedMetadata);
        std::wstring cachedFormat = cachedMetadata.Format;
        std::transform(
            cachedFormat.begin(), cachedFormat.end(),
            cachedFormat.begin(), ::towupper);
        const uint64_t cachedPixels =
            static_cast<uint64_t>(cachedMetadata.Width) *
            static_cast<uint64_t>(cachedMetadata.Height);
        const bool requiresDispatchState =
            cachedFormat == L"JXL" ||
            cachedFormat.contains(L"RAW") ||
            cachedMetadata.Width > 8192 ||
            cachedMetadata.Height > 8192 ||
            cachedPixels > 50000000ULL;
        if (cachedFrame && !requiresDispatchState) {
            EngineEvent event;
            event.type = EventType::FullReady;
            event.filePath = path;
            event.imageId = imageId;
            event.targetSlot = targetSlot;
            event.generationId = generationId;
            event.rawFrame = std::move(cachedFrame);
            event.metadata = std::move(cachedMetadata);
            QueueEvent(std::move(event));
            return;
        }
    }
    // 1. Peek Header
    CImageLoader::ImageHeaderInfo info = m_loader->PeekHeader(path.c_str());

    // [Header Guard] Some containers (large AVIF/HEIC/TIFF variants) may return 0x0 in PeekHeader.
    // Run a second fast probe, then a last-resort WIC size query to stabilize Titan dispatch.
    if (info.width <= 0 || info.height <= 0 || info.format == L"Unknown") {
        CImageLoader::ImageInfo fastInfo{};
        if (SUCCEEDED(m_loader->GetImageInfoFast(path.c_str(), &fastInfo))) {
            if (info.width <= 0 && fastInfo.width > 0) info.width = (int)fastInfo.width;
            if (info.height <= 0 && fastInfo.height > 0) info.height = (int)fastInfo.height;
            if (info.format == L"Unknown" && !fastInfo.format.empty()) info.format = fastInfo.format;
            if (info.fileSize == 0 && fastInfo.fileSize > 0) info.fileSize = fastInfo.fileSize;
        }
        if (info.width <= 0 || info.height <= 0) {
            UINT w = 0, h = 0;
            if (SUCCEEDED(m_loader->GetImageSize(path.c_str(), &w, &h))) {
                info.width = (int)w;
                info.height = (int)h;
            }
        }
    }

    // Force refresh was consumed before the cache fast path above.
    
    // [Titan] Initialize Tile Scheduler
    // Threshold: > 8K in any dimension OR > 50MP total?
    // Let's start conservative: Only explicit Titan triggering for now?
    // No, logic should be automatic.
    // For V1 Titan: Enable for images > 8192 on either side AND supported format (JPEG).
    // TODO: Add Wuffs/LibRaw support for PNG/WebP/Raw tiling in V2.
    std::wstring fmtUpper = info.format;
    std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::toupper);
    
    // [Map First] Create MMF immediately for this image
    // This allows Zero-Copy decoding for Base Layer AND Zero-Copy for Tiles
    std::shared_ptr<QuickView::MappedFile> primaryMMF = std::make_shared<QuickView::MappedFile>(path);
    if (!primaryMMF->IsValid()) primaryMMF.reset(); // Fallback if map fails

    // [Titan] Trigger Conditions
    // 1. Format support: JPEG, WebP, PNG, JXL, TIFF, AVIF
    bool isSupportedFormat = (fmtUpper == L"JPEG" || fmtUpper == L"JPG" || 
                              fmtUpper == L"WEBP" || fmtUpper == L"PNG" || 
                              fmtUpper == L"JXL" || fmtUpper == L"TIF" || 
                              fmtUpper == L"TIFF" || fmtUpper == L"AVIF");

    // 2. Size triggers: Any side > 8192 OR Total pixels > 50MP
    bool sizeTrigger = (info.width > 8192 || info.height > 8192);
    size_t pixelCount = (size_t)info.width * info.height;
    bool pixelTrigger = (pixelCount > 50000000);
    bool unknownDims = (info.width <= 0 || info.height <= 0);
    uintmax_t observedFileSize = (info.fileSize > 0) ? info.fileSize : fileSize;
    bool fallbackFileTrigger = unknownDims && observedFileSize >= (32ull * 1024ull * 1024ull);

    bool enableTitan = (sizeTrigger || pixelTrigger || fallbackFileTrigger) && isSupportedFormat;
    

    if (enableTitan) {
         // [Phase 5] Keep MMF alive for Tile Manager usage. If old MMF exists, send to GC.
         if (m_mmf) {
             HeavyLanePool::TrashBag bag;
             bag.mmf = std::move(m_mmf);
             m_heavyPool->EnqueueTrash(std::move(bag));
         }
         m_mmf = primaryMMF;
         
         m_tileManager->InvalidateAll(); // Reset generation
         QV_LOG("Dispatch_Titan",
             TraceLoggingString("Enabled", "Action"),
             TraceLoggingInt32(info.width, "Width"),
             TraceLoggingInt32(info.height, "Height"),
             TraceLoggingBool(m_mmf && m_mmf->IsValid(), "MMF_OK"));
         
         // [Scientific 2.0] Enable Titan Mode - pool handles dynamic concurrency via Scout phase.
         // SetTitanMode(true) resets scout state, sets initial concurrency to 2, 
         // and after measuring 2 tiles, adjusts to optimal thread count based on MP/s.
         m_heavyPool->SetTitanMode(true, info.width, info.height, fmtUpper, imageId);
         m_heavyPool->SetUseThreadLocalHandle(true);
         m_enablePadding = true;

    } else {
         // [Phase 5] old MMF is destructed via GC thread
         if (m_mmf) {
             HeavyLanePool::TrashBag bag;
             bag.mmf = std::move(m_mmf);
             m_heavyPool->EnqueueTrash(std::move(bag));
         }
         
         // [Fix] Deactivate Titan Mode (Elastic)
         m_heavyPool->SetTitanMode(false, 0, 0, L"", 0);
         m_enablePadding = true; 
    }

    // [Prefetch System] Cache Check ...
    // If the image is already in memory (from prefetch), use it immediately!
    {
        CImageLoader::ImageMetadata cachedMetadata;
        auto cachedFrame = GetCachedImage(path, &cachedMetadata);

        // Guard: do not reuse JXL placeholder/scaled cache as a final hit.
        // Large JXL revisit must re-enter decode pipeline to ensure proper tile activation.
        if (cachedFrame && fmtUpper == L"JXL") {
            const bool frameUsable =
                cachedFrame->IsValid() &&
                !cachedFrame->IsSvg();
            const bool tinyFakeBase =
                frameUsable &&
                (cachedFrame->width <= 1 || cachedFrame->height <= 1);
            const bool hasMetaDims = (info.width > 0 && info.height > 0);
            const bool scaledPreviewCache =
                frameUsable && hasMetaDims &&
                (cachedFrame->width < (int)(info.width * 0.85f) ||
                 cachedFrame->height < (int)(info.height * 0.85f));
            const bool cacheUnknownDims = !hasMetaDims;
            const bool bypassJxlCache = tinyFakeBase || scaledPreviewCache || cacheUnknownDims;

            if (bypassJxlCache) {
                const int cacheW = frameUsable ? cachedFrame->width : 0;
                const int cacheH = frameUsable ? cachedFrame->height : 0;
                InvalidateCache(path);
                cachedFrame.reset();
                QV_LOG("Dispatch_JXLCache",
                    TraceLoggingString("Bypass", "Action"),
                    TraceLoggingInt32(cacheW, "CacheW"),
                    TraceLoggingInt32(cacheH, "CacheH"),
                    TraceLoggingInt32(info.width, "MetaW"),
                    TraceLoggingInt32(info.height, "MetaH"),
                    TraceLoggingBool(enableTitan, "Titan"));
            }
        }

        if (cachedFrame) {
            bool isHit = true;
            
            // [v9.0] Smart RAW Quality Check
            // RAW files require strict quality matching (Preview vs Full) for A/B comparison
            if (info.format.contains(L"RAW")) {
                  bool wantFull = m_config.ForceRawDecode;
                  bool hasFull = (cachedFrame->quality == QuickView::DecodeQuality::Full);
                  
                  // [Fix] Logic Relaxed: Only invalidate if we WANT full but DON'T have it.
                  // If we want Preview (wantFull=false) but have Full (hasFull=true), that's a BONUS. 
                  // Don't reload Preview, use the high-quality cached one.
                  if (wantFull && !hasFull) {
                       isHit = false;
                       // Explicitly invalidate so new load can replace it
                       InvalidateCache(path); 
                  }
            }

            if (isHit) {
                EngineEvent e;
                e.type = EventType::FullReady;
                e.filePath = path; 
                e.imageId = imageId;
                e.targetSlot = targetSlot;
                e.generationId = generationId;
                e.rawFrame = cachedFrame; // Zero-copy shared_ptr
                
                // Metadata and histogram were computed during the original
                // decode. Reuse them; rescanning 4K pixels here turned every
                // cache hit into another ~70 ms navigation stall.
                e.metadata = std::move(cachedMetadata);
                if (e.metadata.Width == 0) {
                    e.metadata.Width =
                        (cachedFrame->srcWidth > 0) ? cachedFrame->srcWidth : cachedFrame->width;
                }
                if (e.metadata.Height == 0) {
                    e.metadata.Height =
                        (cachedFrame->srcHeight > 0) ? cachedFrame->srcHeight : cachedFrame->height;
                }
                if (e.metadata.Format.empty()) e.metadata.Format = info.format;
                if (e.metadata.FileSize == 0) e.metadata.FileSize = info.fileSize;
                if (e.metadata.FormatDetails.empty()) {
                    e.metadata.FormatDetails = cachedFrame->formatDetails;
                }
                e.metadata.colorInfo = cachedFrame->colorInfo;
                e.metadata.hdrMetadata = cachedFrame->hdrMetadata;
                if (!e.metadata.HasEmbeddedColorProfile.has_value()) {
                    e.metadata.HasEmbeddedColorProfile =
                        cachedFrame->colorInfo.hasEmbeddedIcc || !cachedFrame->iccProfile.empty();
                }
                if (cachedFrame->IsSvg()) e.metadata.Format = L"SVG";
                e.metadata.ExifOrientation = cachedFrame->exifOrientation;
                
                QueueEvent(std::move(e)); 
                
                return; 
            }
        }
    }
    
    // [DEBUG] Log
    {
        QV_LOG("Dispatch_Route",
            TraceLoggingInt32(info.width, "Width"),
            TraceLoggingInt32(info.height, "Height"),
            TraceLoggingFloat64((double)(info.width * info.height) / 1000000.0, "Megapixels"));
    }
    
    // Update State for UI
    m_hasEmbeddedThumb = info.hasEmbeddedThumb;
    
    bool useFastLane = m_config.EnableScout;
    bool useHeavy = m_config.EnableHeavy;
    
    // [Titan] Compliance: Force Heavy Lane for Base Layer
    // [Titan] Compliance: Force Heavy Lane for Base Layer
    if (enableTitan) {
        useHeavy = true;
        useFastLane = false; 
        
        // [Titan] >200MP images use the same base decode path as other Titan images.
        // IDCT 1/8 scaling produces ~3-8MP preview (sufficient for 4K screens).
        // Tiles are triggered by main.cpp OnPaint only when zoom > basePreviewRatio.

        QV_LOG("Dispatch_Route", TraceLoggingString("Titan HeavyLane", "Action"));
    }
    
    // 2. Recursive RAW Check
    // If it's a RAW file with an embedded thumb, check the preview resolution.
    // "RAW" detection: check if string contains RAW or format check from loader
    if (info.format.contains(L"RAW")) {
        // [v9.9] If ForceRawDecode is enabled, RAW Full Decode is computationally intensive.
        // Always route to HeavyLane to avoid blocking FastLane (UI thread responsiveness).
        // [Fix] Use member config!
        if (m_config.ForceRawDecode) {
            // [Fix] Explicitly request Full Decode
            // This ensures we bypass IDCT scaling and get the full sensor resolution
            m_heavyPool->SubmitFullDecode(path, imageId, primaryMMF, targetSlot, generationId);
            return; 
        }
        else if (info.hasEmbeddedThumb) {
            int embW = 0, embH = 0;
            // Call the new method [v6.5 Recursor]
            if (SUCCEEDED(m_loader->GetEmbeddedPreviewInfo(path.c_str(), &embW, &embH))) {
                uint64_t embPixels = (uint64_t)embW * embH;
                // Threshold: 2.5 MP (Conservative)
                // If embedded preview is huge, it will block FastLane. Force Heavy Lane.
                if (embPixels > 2500000) { 
                    QV_LOG("Dispatch_Route", TraceLoggingString("RAW EmbeddedPreview TooLarge", "Action"));
                    // Override Classification: Treat as Heavy
                    info.type = CImageLoader::ImageType::TypeB_Heavy; 
                }
            }
        }
    }
    
    // 3. Classification Logic (Refined)
    if (info.type == CImageLoader::ImageType::TypeA_Sprint) {
        // Small/Fast images -> FastLane Only
        // [v4.1] Exception: JXL (TypeA) uses Heavy/serial logic
        if (info.format != L"JXL" && info.format != L"WebP") {
             useHeavy = false;
        }
        
        // [v7.1] WebP Strategy: Strict Threshold
        // < 1.5MB and < 2MP -> FastLane (Memory/CPU Cheap)
        // Else -> HeavyLane (Direct Full Decode, Background)
        std::wstring fmtLower = info.format;
        std::transform(fmtLower.begin(), fmtLower.end(), fmtLower.begin(), ::towlower);
        
        if (fmtLower == L"webp") {
             bool isSmall = (info.fileSize < 1572864) && // 1.5 * 1024 * 1024
                            ((uint64_t)info.width * info.height < 2000000);
             if (isSmall) {
                 useFastLane = true;
                 useHeavy = false;
             } else {
                // [v7.2 Fix] Large WebP -> Force Heavy Direct (non-Titan is full decode).
                 QV_LOG("Dispatch_Route", TraceLoggingString("WebP Large HeavyDirect", "Action"));
                 m_heavyPool->Submit(path, imageId, primaryMMF, targetSlot, generationId);
                 return; 
             }
        }
    } 
    else if (info.type == CImageLoader::ImageType::TypeB_Heavy) {
        // Large Image -> Heavy Only (FastLane ignored)
        if (!enableTitan) useFastLane = false; 
        
        // [v7.2 Fix] Check WebP Heavy here too (if flagged as TypeB by PeekHeader)
        std::wstring fmtLower = info.format;
        std::transform(fmtLower.begin(), fmtLower.end(), fmtLower.begin(), ::towlower);
        
        if (fmtLower == L"webp") {
             QV_LOG("Dispatch_Route", TraceLoggingString("WebP Heavy HeavyDirect", "Action"));
             m_heavyPool->Submit(path, imageId, primaryMMF, targetSlot, generationId); // Base Layer Scaled
             return;
        }
        
        // Other heavy formats use standard Heavy path (full decode for non-Titan)
    }
    
    // 5. JXL Special Logic (User "Ultimate Strategy")
    if (info.format == L"JXL") {
        m_pendingJxlHeavyPath.clear();
        m_pendingJxlHeavyId = 0;
        m_pendingJxlHeavySlot = targetSlot;
        m_pendingJxlHeavyGenerationId = generationId;
        
        // Scene A: Small JXL (< 1MB AND < 2MP) -> FastLane Direct Full Decode
        // 1MB = 1048576 bytes
        // 2MP = 2000000 pixels
        // [v9.2] Fix: Check for valid dimensions! (PeekHeader fail = 0x0)
        bool isSmall = (info.fileSize < 1048576) && 
                       (info.width > 0 && info.height > 0) &&
                       ((uint64_t)info.width * info.height < 2000000);
        
        if (isSmall) {
            QV_LOG("Dispatch_Route", TraceLoggingString("JXL Small FastLane", "Action"));
            // FastLane will use target=0 if detected as small
            m_fastLane.Push(path, imageId, m_targetHdrHeadroomStops.load(std::memory_order_relaxed), targetSlot, generationId);
        } 
        else {
            if (enableTitan) {
                // [v8.5] Hard Dispatch: Large JXL (>2MP or >3MB)
                // Skip FastLane entirely. HeavyLane handles everything (Deep Cancel Relay).
                QV_LOG("Dispatch_Route", TraceLoggingString("JXL Titan HeavyDirect", "Action"));
                
                // [Fix] Stage 2 Trigger explicitly needs these to be set for the pending heavy decode
                m_pendingJxlHeavyPath = path;
                m_pendingJxlHeavyId = imageId;
                m_pendingJxlHeavySlot = targetSlot;
                m_pendingJxlHeavyGenerationId = generationId;
                
                m_heavyPool->Submit(path, imageId, primaryMMF, targetSlot, generationId);
            } else {
                // [v3.2.5 Restore] Ordinary non-Titan large images, run FullDecode directly like the old version
                // Eliminates 300ms latency, fastest speed, and naturally shows the embedded preview from the decoder!
                QV_LOG("Dispatch_Route", TraceLoggingString("JXL Large HeavyFullDecode", "Action"));
                m_heavyPool->SubmitFullDecode(path, imageId, primaryMMF, targetSlot, generationId);
            }
        }
        return; // JXL dispatched
    }
    
    // 6. Specialized Dispatch for TIFF/HEIC/HEIF/AVIF/PSD/HDR/PIC/PCX/EXR/JXR (30ms budget optimization)
    if (info.format == L"TIFF" || info.format == L"HEIC" || info.format == L"HEIF" || info.format == L"AVIF" ||
        info.format == L"PSD"  || info.format == L"HDR"  || info.format == L"PIC"  || info.format == L"PCX" ||
        info.format == L"EXR"  || info.format == L"JXR") {
        uint64_t pixels = (uint64_t)info.width * info.height;
        bool isSmall = false;

        if (info.format == L"TIFF") {
            // TIFF: < 5MP and < 20MB
            // Uncompressed 5MP is fast. Large compressed TIFFs are slow.
            // [Fix] Ensure pixels > 0 (if header parse failed, assume large)
            isSmall = (pixels > 0 && pixels < 5000000 && info.fileSize < 20971520);
        } 
        else if (info.format == L"PSD") {
            // PSD: < 2.1MP and < 5MB
            // Parsing layers can be very slow. Even small resolution composite might be slow if file is huge.
            isSmall = (pixels > 0 && pixels <= 2100000 && info.fileSize < 5242880);
        }
        else if (info.format == L"HDR" || info.format == L"PIC") {
            // HDR/PIC: < 2.1MP
            // Radiance RGBE decoding is float-based, slightly slower than 8-bit RLE.
            isSmall = (pixels > 0 && pixels <= 2100000);
        }
        else if (info.format == L"PCX") {
            // PCX: < 3MP
            // Simple RLE, CPU bound but generally fast. slightly looser threshold.
            isSmall = (pixels > 0 && pixels <= 3000000);
        }
        else if (info.format == L"EXR") {
            // EXR: < 2.1MP
            // OpenEXR/TinyEXR involves decompression and float16 conversion.
            isSmall = (pixels > 0 && pixels <= 2100000);
        }
        else {
            // HEIC/HEIF/AVIF/JXR: < 2.1MP (FHD)
            // Compute intensive or WIC high-precision. Limit to FHD for FastLane.
            // [Fix] Ensure pixels > 0. PeekHeader fails for some containers/codecs,
            // so width=0. We MUST treat unknown size as Large to prevent blocking UI.
            isSmall = (pixels > 0 && pixels <= 2100000);
        }

        if (isSmall) {
            QV_LOG("Dispatch_Route", TraceLoggingString("FormatSmall FastLane", "Action"));
            m_fastLane.Push(path, imageId, m_targetHdrHeadroomStops.load(std::memory_order_relaxed), targetSlot, generationId);
        } else {
            QV_LOG("Dispatch_Route", TraceLoggingString("FormatLarge HeavyLane", "Action"));
            m_heavyPool->Submit(path, imageId, primaryMMF, targetSlot, generationId);
        }
        return;
    }

    // 7. Standard Routing
    if (useHeavy) {
        QV_LOG("Dispatch_Route", TraceLoggingString("HeavyLane", "Action"));
        m_heavyPool->Submit(path, imageId, primaryMMF, targetSlot, generationId);
    }
    if (useFastLane) {
        // Avoid parallel duplicate work if Heavy is already taking it?
        // Logic: TypeA -> FastLane only. TypeB -> Heavy only.
        // Unknown type -> Parallel (Both).
        QV_LOG("Dispatch_Route", TraceLoggingString("FastLane", "Action"));
        m_fastLane.Push(path, imageId, m_targetHdrHeadroomStops.load(std::memory_order_relaxed), targetSlot, generationId);
    }
}

void ImageEngine::NavigateTo(const std::wstring& path, uintmax_t fileSize, uint64_t navToken, PaneSlot targetSlot, uint64_t generationId) {
    if (path.empty()) return;
    
    // [Phase 3] Store the navigation token
    m_currentNavToken.store(navToken);
    
    // [ImageID Architecture] Compute stable hash
    ImageID imageId = ComputePathHash(path);
    m_currentImageId.store(imageId);
    m_currentImageIdBySlot[static_cast<int>(targetSlot)].store(imageId);
    // Keep speculative lookahead decodes alive while cancelling unrelated
    // stale work. Killing these jobs on every key repeat made prefetch restart
    // continuously and forced each navigation back onto the decode path.
    std::vector<ImageID> protectedPrefetchIds;
    {
        std::lock_guard lock(m_pendingMutex);
        protectedPrefetchIds.reserve(m_pendingPaths.size());
        for (const auto& pendingPath : m_pendingPaths) {
            protectedPrefetchIds.push_back(ComputePathHash(pendingPath));
        }
    }
    m_heavyPool->CancelOthers(imageId, targetSlot, protectedPrefetchIds);
    
    m_currentNavPath = path;
    m_lastInputTime = std::chrono::steady_clock::now();

    // [JXL Serial] Reset State
    m_isViewingScaledImage = false;
    m_stage2Requested = false;
    m_baseLayerReady.store(false);

    // Use Central Dispatcher
    DispatchImageLoad(path, imageId, fileSize, targetSlot, generationId);
}

ImageID ImageEngine::GetCurrentImageId(PaneSlot slot) const {
    return m_currentImageIdBySlot[static_cast<int>(slot)].load();
}

bool ImageEngine::ShouldSkipFastLaneForFastFormat(const std::wstring& path) {
    // Check extension
    std::wstring ext = path;
    size_t dot = ext.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    
    std::wstring e = ext.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    
    // Fast formats where Wuffs decode is faster than WIC thumbnail
    bool isFastFormat = (e == L".png" || e == L".apng" || e == L".gif" || e == L".bmp" ||
                         e == L".tga" || e == L".wbmp" || e == L".qoi" ||
                         e == L".ppm" || e == L".pgm" || e == L".pbm" || e == L".pam");
    if (!isFastFormat) return false;
    
    // Check image size - skip FastLane only for small images
    UINT w = 0, h = 0;
    if (SUCCEEDED(m_loader->GetImageSize(path.c_str(), &w, &h))) {
        // < 16 Megapixels (4096x4096)
        if (w * h < 16 * 1024 * 1024) {
            return true; // Skip FastLane, Heavy Lane will handle
        }
    }
    return false;
}

std::vector<EngineEvent> ImageEngine::PollState() {
    std::vector<EngineEvent> batch;
    
    // 0. Harvest Manual Events (Cache Hits)
    {
        std::lock_guard lock(m_manualQueueMutex);
        if (!m_manualEventQueue.empty()) {
            batch.insert(batch.end(), std::make_move_iterator(m_manualEventQueue.begin()), std::make_move_iterator(m_manualEventQueue.end()));
            m_manualEventQueue.clear();
        }
    }
    
    // 1. Harvest FastLane Events
    while (auto e = m_fastLane.TryPopResult()) {
        batch.push_back(std::move(*e));
    }

    // 2. Harvest Heavy Events
    while (auto e = m_heavyPool->TryPopResult()) {
         if (e->type == EventType::TileReady && e->tileCoord.has_value() && e->rawFrame) {
             // [Fix] Only route tiles belonging to the CURRENT image to TileManager.
             // Stale tiles from a previous image (decoded after navigation) must be
             // discarded here to prevent old tile data overlaying the new image's preview.
             if (e->imageId == m_currentImageId.load()) {
                 auto key = QuickView::TileKey::From(e->tileCoord->col, e->tileCoord->row, e->tileCoord->lod);
                 m_tileManager->OnTileReady(key, e->rawFrame);
             }
             
             // Still pass to batch? 
             // Yes, Main Thread might want to trigger Repaint.
             // But UI doesn't need to know details.
         } else if (e->type == EventType::FullReady && e->imageId == m_currentImageId.load()) {
             m_pool.SwapHeavy();
         }
         batch.push_back(std::move(*e));
    }

    // 3. [JXL Serial] Track state and trigger full decode
    for (const auto& e : batch) {
        if (e.imageId == m_currentImageId.load()) {
            if ((e.type == EventType::PreviewReady || e.type == EventType::FullReady) &&
                e.rawFrame && e.rawFrame->IsValid()) {
                m_baseLayerReady.store(true);
            }

            if (e.type == EventType::PreviewReady || (e.type == EventType::FullReady && e.isScaled)) {
                m_isViewingScaledImage = true;
                m_stage1Time = std::chrono::steady_clock::now();



                // [JXL Serial] Trigger Stage 2 IMMEDIATELY for JXL (No 300ms wait)
                if (m_pendingJxlHeavyId == e.imageId && m_pendingJxlHeavyId != 0) {
                     QV_LOG("PollState_Route", TraceLoggingString("JXL PreviewReady HeavyImmediate", "Action"));
                     RequestFullDecode(m_pendingJxlHeavyPath, m_pendingJxlHeavyId, m_pendingJxlHeavySlot, m_pendingJxlHeavyGenerationId);
                     m_stage2Requested = true; 
                     m_pendingJxlHeavyId = 0; 
                }

            } else if (e.type == EventType::FullReady && !e.isScaled) {
                m_isViewingScaledImage = false; // Final reached
                m_stage2Requested = false;      // Reset request flag (job done)
                


                // [JXL Scene C] FastLane Aborted (Modular?) -> Trigger Heavy Immediately
                if (m_pendingJxlHeavyId == e.imageId && m_pendingJxlHeavyId != 0) {
                     QV_LOG("PollState_Route", TraceLoggingString("FastLane Failed HeavyImmediate", "Action"));
                     RequestFullDecode(m_pendingJxlHeavyPath, m_pendingJxlHeavyId, m_pendingJxlHeavySlot, m_pendingJxlHeavyGenerationId);
                     m_stage2Requested = true; // Mark as requested
                     m_pendingJxlHeavyId = 0;  // Consumed
                }
            }
        }
        

        
        // [v9.2] Fix: Clean up pending paths on Error too (Fixes Blue Light Forever)
        if (e.type == EventType::LoadError) {
             // [HEIC] Special handling for missing codec: Trigger install prompt on UI thread
             // [v10.5 Fix] Only trigger for formats that actually depend on the HEVC/AV1 extensions
             std::wstring formatUpper = e.metadata.Format;
             std::transform(formatUpper.begin(), formatUpper.end(), formatUpper.begin(), ::towupper);
             bool isHevcDependent = (formatUpper == L"HEIC" || formatUpper == L"HEIF" || formatUpper == L"AVIF");
             
             if (isHevcDependent && CImageLoader::ImageMetadata::IsWicCodecMissing(e.hr)) {
                  QV_LOG("Engine_CodecMissing",
                      TraceLoggingUInt32((uint32_t)e.hr, "HR"));
                  PostMessage(m_hwnd, WM_APP + 99, 0, 0);
             }
             std::lock_guard lock(m_pendingMutex);
             m_pendingPaths.erase(e.filePath);
        }

        // [v8.11 Fix] Cache ALL FullReady events (both current and prefetch)
        // Previously, only non-current (prefetch) images were cached.
        // This caused the bug where viewed images weren't cached for return navigation.
        if (e.type == EventType::FullReady && e.rawFrame && e.rawFrame->IsValid()) {
            std::wstring fmtUpper = e.metadata.Format;
            std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::towupper);
            const bool isJxl = (fmtUpper == L"JXL");

            // For large JXL, do not cache placeholders or strongly downscaled base frames.
            const bool hasMetaDims = (e.metadata.Width > 0 && e.metadata.Height > 0);
            const bool isLargeJxl =
                isJxl && hasMetaDims &&
                (e.metadata.Width >= 8000 || e.metadata.Height >= 8000 ||
                 ((uint64_t)e.metadata.Width * (uint64_t)e.metadata.Height > 50000000ULL));
            const bool tinyFakeBase =
                isJxl && !e.rawFrame->IsSvg() &&
                (e.rawFrame->width <= 1 || e.rawFrame->height <= 1);
            const bool downscaledJxlBase =
                isLargeJxl && !e.rawFrame->IsSvg() &&
                (e.rawFrame->width < (int)(e.metadata.Width * 0.85f) ||
                 e.rawFrame->height < (int)(e.metadata.Height * 0.85f));
            const bool skipJxlCache = tinyFakeBase || downscaledJxlBase;

            if (skipJxlCache) {
                std::lock_guard lock(m_pendingMutex);
                m_pendingPaths.erase(e.filePath);
            } else {
                if (m_navigator) {
                    const int idx = m_navigator->FindIndex(e.filePath);
                    if (idx != -1) {
                        AddToCache(idx, e.filePath, e.rawFrame, e.metadata);
                    }
                }
                // FullReady is terminal even when the navigator changed before
                // the result arrived; never leave a phantom pending marker.
                std::lock_guard lock(m_pendingMutex);
                m_pendingPaths.erase(e.filePath);
            }
        }
    }
    
    // Check Timer (300ms idle)
    // Only if pending JXL is waiting and not already requested
    if (m_isViewingScaledImage && !m_stage2Requested && m_pendingJxlHeavyId != 0 && m_pendingJxlHeavyId == m_currentImageIdBySlot[static_cast<int>(m_pendingJxlHeavySlot)].load()) {
        auto now = std::chrono::steady_clock::now();
        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_stage1Time).count();
        if (dur > 300) {
             RequestFullDecode(m_currentNavPath, m_currentImageIdBySlot[static_cast<int>(m_pendingJxlHeavySlot)].load(), m_pendingJxlHeavySlot, m_pendingJxlHeavyGenerationId);
             m_stage2Requested = true;
             m_pendingJxlHeavyId = 0; // Consumed
        }
    }

    // [v9.1] Pump Serial Prefetch Queue
    PumpPrefetch();

    return batch;
}

bool ImageEngine::IsIdle() const {
    return m_fastLane.IsQueueEmpty() && m_heavyPool->IsIdle();
}

ImageEngine::DebugStats ImageEngine::GetDebugStats() const {
    DebugStats s = {};
    s.fastQueueSize = m_fastLane.GetQueueSize();
    s.fastResultsSize = m_fastLane.GetResultsSize();
    
    auto poolStats = m_heavyPool->GetStats();
    s.heavyState = m_heavyPool->IsBusy() ? HeavyState::DECODING : HeavyState::IDLE;
    s.cancelCount = poolStats.cancelCount;
    s.heavyPendingCount = poolStats.pendingJobs;
    s.heavyDecodeTimeMs = poolStats.lastDecodeTimeMs; 
    s.heavyLastImageId = poolStats.lastDecodeId; // [HUD Fix]
    
    // Memory
    s.memoryUsed = m_pool.GetUsedMemory();
    s.memoryTotal = m_pool.GetTotalMemory();
    
    s.fastSkipCount = m_fastLane.GetSkipCount();
    s.fastTotalTimeMs = m_fastLane.m_lastTotalTimeMs.load(); 
    s.fastDecodeTimeMs = m_fastLane.m_lastDecodeTimeMs.load();
    s.fastLastImageId = m_fastLane.m_lastLoadId.load();
    s.fastDroppedCount = m_fastLane.m_droppedCount.load();
    s.fastWorking = m_fastLane.m_isWorking.load();


    // Fallback loader name
    if (s.loaderName.empty()) {
        s.loaderName = m_fastLane.GetLastLoaderName();
    }
    if (s.loaderName.empty()) {
        s.loaderName = L"[Unknown]";
    }
    
    {
        std::lock_guard lock(m_cacheMutex);
        s.cacheMemoryUsed = m_currentCacheBytes;
        
        for (int i = -TOPOLOGY_RANGE; i <= TOPOLOGY_RANGE; ++i) {
            int slotIndex = i + TOPOLOGY_RANGE; // Convert to 0-4 array index
            int targetIndex = m_currentViewIndex + i;
            
            if (!m_navigator || targetIndex < 0 || targetIndex >= (int)m_navigator->Count()) {
                s.topology.slots[slotIndex] = CacheStatus::EMPTY;
                continue;
            }
            
            // Special handling for CUR (current image)
            if (i == 0) {
                // Check Heavy Lane state for current image
                if (s.heavyState == HeavyState::IDLE && s.heavyDecodeTimeMs > 0) {
                    // Heavy finished = full image ready
                    s.topology.slots[slotIndex] = CacheStatus::HEAVY;
                } else if (s.heavyState == HeavyState::DECODING) {
                    // Heavy in progress = pending
                    s.topology.slots[slotIndex] = CacheStatus::PENDING;
                } else {
                    // FastLane only mode or not yet loaded
                    s.topology.slots[slotIndex] = CacheStatus::FAST;
                }
                continue;
            }
            
            // For neighbors, check prefetch cache
            const std::wstring& path = m_navigator->GetFile(targetIndex);
            auto it = m_cache.find(path);
            
            if (it != m_cache.end()) {
                s.topology.slots[slotIndex] = CacheStatus::HEAVY;
            } else {
                s.topology.slots[slotIndex] = CacheStatus::EMPTY;
            }
        }
    }
    
    // Phase 4: Arena Water Levels
    // Use GetLastArenaBytes() which tracks decoded.pixels.size()
    // TODO: Get arena stats from pool workers
    s.arena.activeCapacity = m_pool.GetTotalCapacity();
    s.arena.backCapacity = m_pool.GetTotalUsed();

    return s;
}

// [HUD V4] Zero-Cost Telemetry Gathering (Pull Mode)
ImageEngine::TelemetrySnapshot ImageEngine::GetTelemetry() const {
    TelemetrySnapshot s = {};
    
    // 1. Vitals (Zone A & A2)
    s.targetHash = m_currentImageId.load();
    s.baseLayerReady = m_baseLayerReady.load();
    // RenderHash is filled by UI (main.cpp)
    // FPS is filled by UI
    // Loader Name
    wcscpy_s(s.loaderName, m_fastLane.GetLastLoaderName().c_str());
    
    // Legacy DComp Lights: Filled by UI
    
    // Zone B: Matrix (FastLane)
    s.fastQueue = m_fastLane.GetQueueSize();
    s.fastDropped = m_fastLane.m_droppedCount.load();
    s.fastWorking = m_fastLane.m_isWorking.load();
    // [Phase 10] Filter FastLane Time by ImageID
    // [Dual Timing] Return both decode and total times
    if (m_fastLane.m_lastLoadId == s.targetHash) {
        s.fastDecodeTime = (int)m_fastLane.m_lastDecodeTimeMs.load();
        s.fastTotalTime = (int)m_fastLane.m_lastTotalTimeMs.load();
    } else {
        s.fastDecodeTime = 0;
        s.fastTotalTime = 0;
    }
    
    // [Phase 10] Pass targetHash to filter stale times
    m_heavyPool->GetWorkerSnapshots((HeavyLanePool::WorkerSnapshot*)s.heavyWorkers, 16, &s.heavyWorkerCount, s.targetHash);
    
    // [HUD V4] Get global pool stats for Cancellation Count
    HeavyLanePool::PoolStats poolStats = m_heavyPool->GetStats();
    s.heavyCancellations = poolStats.cancelCount;
    s.heavyPendingJobs = poolStats.pendingJobs;
    s.activeTileJobs = poolStats.activeTileJobs;
    s.masterWarmupActive = poolStats.masterWarmupActive;

    s.heavyBusyWorkers = 0;
    for (int i = 0; i < s.heavyWorkerCount; ++i) {
        if (s.heavyWorkers[i].busy) s.heavyBusyWorkers++;
    }

    if (m_tileManager) {
        auto vp = m_tileManager->GetViewportProgress();
        s.tileCount = vp.totalTiles;
        s.tilesReady = vp.readyTiles;
    }
    s.isTitan = (s.activeTileJobs > 0 || s.tileCount > 0);

    // [Phase 11] Bubble up Heavy Lane Loader Name
    bool hasFullDecode = false;
    for (int i = 0; i < s.heavyWorkerCount; ++i) {
        // If worker has a valid result
        // [Dual Timing] Check both times
        if ((s.heavyWorkers[i].lastDecodeMs > 0 || s.heavyWorkers[i].lastTotalMs > 0) && s.heavyWorkers[i].loaderName[0] != 0) {
            
            // Priority: Full Decode > Scaled Decode > Scout
            if (s.heavyWorkers[i].isFullDecode) {
                wcscpy_s(s.loaderName, s.heavyWorkers[i].loaderName);
                hasFullDecode = true;
                break; // Found the best quality, stop searching
            }
            
            // If we haven't found a full decode yet, take this (likely scaled) result
            if (!hasFullDecode) {
                // [UI Fix] Priority: Tile Decode > Scaled Base Layer
                if (s.heavyWorkers[i].isTileDecode || s.loaderName[0] == 0) {
                    wcscpy_s(s.loaderName, s.heavyWorkers[i].loaderName);
                }
            }
        }
    }
    
    // 3. Logic (Zone C)
    // Reconstruct topology from cache
    {
        std::lock_guard cacheLock(m_cacheMutex);
        
        // [v8.15] Use atomic loads for thread safety
        int curViewIdx = m_currentViewIndex.load();
        int lastDir = m_lastDirectionInt.load();
        
        // [v8.12] Populate prefetch state for HUD
        s.prefetchEnabled = m_prefetchPolicy.enablePrefetch;
        s.prefetchLookAhead = m_prefetchPolicy.enablePrefetch ? m_prefetchPolicy.lookAheadCount : 0;
        s.browseDirection = lastDir;
        
        static constexpr int OFFSET = TelemetrySnapshot::TOPO_OFFSET;
        
        // [v8.15] Lock pending set for checking
        std::lock_guard pendingLock(m_pendingMutex);
        
        for (int i = 0; i < 32; ++i) {
             int relOffset = i - OFFSET;
             int targetIndex = curViewIdx + relOffset;
             
             if (!m_navigator || targetIndex < 0 || targetIndex >= (int)m_navigator->Count()) {
                 s.cacheSlots[i] = CacheStatus::EMPTY;
                 continue;
             }
             
             std::wstring path = m_navigator->GetFile(targetIndex);
             if (m_cache.count(path)) {
                 s.cacheSlots[i] = CacheStatus::HEAVY; // Green (Cached)
             } else if (m_pendingPaths.count(path)) {
                 s.cacheSlots[i] = CacheStatus::PENDING; // Blue (Processing)
             } else {
                 s.cacheSlots[i] = CacheStatus::EMPTY; // Gray (Not loaded)
             }
        }
    }
    
    // 4. Memory (Zone D)
    s.pmrUsed = m_pool.GetUsedMemory();
    s.pmrCapacity = m_pool.GetTotalMemory();
    
    // System Memory
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        s.sysMemory = pmc.WorkingSetSize;
    }
    
    return s;
}

void ImageEngine::ResetDebugCounters() {
    m_fastLane.ResetSkipCount();

}

// ============================================================================
// Fast Lane (The Recon)
// ============================================================================

ImageEngine::FastLane::FastLane(ImageEngine* parent, CImageLoader* loader)
    : m_loader(loader), m_parent(parent)
{
    // Start worker
    m_thread = std::jthread([this]() { QueueWorker(); });
}

ImageEngine::FastLane::~FastLane() {
    m_stopSignal = true;
    m_cv.notify_all();
    // m_thread destructor joins
}

// [v3.1] Ruthless Purge: Clear pending queue but keep results
void ImageEngine::FastLane::Clear() {
    std::lock_guard lock(m_queueMutex);
    m_droppedCount += (int)m_queue.size(); // [HUD V4] Count drops
    m_queue.clear();
    // CRITICAL: Do NOT clear m_results. Completed thumbnails should remain.
    // m_skipCount is not reset here, it accumulates for debug stats.
}

void ImageEngine::FastLane::Push(const std::wstring& path, ImageID id, float targetHdrHeadroomStops, PaneSlot targetSlot, uint64_t generationId) {
    if (m_stopSignal) return;
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back({path, id, targetHdrHeadroomStops, targetSlot, generationId});
        // [v3.1] Simplified Push: No complex anti-explosion here.
        // UpdateView()'s "Ruthless Purge" handles queue depth.
    }
    
    // [DEBUG] Log Push notification
    {
        QV_LOG("FastLane_Push",
            TraceLoggingInt32((int)m_queue.size(), "QueueSize"));
    }

    // [Phase 10] Reset timer logic
    m_lastDecodeTimeMs = 0.0;
    m_lastTotalTimeMs = 0.0;
    
    m_cv.notify_one();
}

std::optional<EngineEvent> ImageEngine::FastLane::TryPopResult() {
    // Ideally we'd use a mutex here too, but for single-consumer (MainThread)
    // we can check empty first or lock.
    // Let's use a try_lock or just lock.
    // Accessing m_results from MainThread, m_queueMutex protects both? 
    // No, let's assume m_queueMutex protects m_queue and m_results.
    std::lock_guard lock(m_queueMutex);
    if (m_results.empty()) return std::nullopt;
    
    auto e = std::move(m_results.front());
    m_results.pop_front();
    return e;
}

bool ImageEngine::FastLane::IsQueueEmpty() const {
    std::lock_guard lock(m_queueMutex);
    return m_queue.empty() && m_results.empty();
}

int ImageEngine::FastLane::GetQueueSize() const {
    std::lock_guard lock(m_queueMutex);
    return (int)m_queue.size();
}

int ImageEngine::FastLane::GetResultsSize() const {
    std::lock_guard lock(m_queueMutex);
    return (int)m_results.size();
}

void ImageEngine::FastLane::QueueWorker() {
    QV_LOG("FastLane_Lifecycle", TraceLoggingString("WorkerStarted", "Action"));

    while (!m_stopSignal) {
        FastLaneCommand cmd;
        {
            {
                std::unique_lock lock(m_queueMutex);
                m_cv.wait(lock, [this] { return m_stopSignal || !m_queue.empty(); });

                if (m_stopSignal && m_queue.empty()) break;

                // [v3.2] Robustness: Check again for empty to be safe
                if (!m_queue.empty()) {
                    cmd = m_queue.front();
                    m_queue.pop_front();
                }
            }
            
            if (cmd.path.empty()) continue;
            
            m_isWorking = true; // [HUD V4] Active
            
            QV_LOG("FastLane_Process", TraceLoggingString("Start", "Action"));

            // --- Work Stage (Unified RawImageFrame Architecture) ---
            auto start = std::chrono::high_resolution_clock::now();
            
            // [Unified Architecture] FastLane uses ScoutArena
            // Reset arena before each task to ensure clean state (FastLane is serial)
            m_parent->m_pool.ResetScout();
            QuantumArena& arena = m_parent->m_pool.GetScoutArena();
            
            QuickView::RawImageFrame rawFrame;
            std::wstring loaderName;
            
            auto info = m_loader->PeekHeader(cmd.path.c_str());
            
            // [Fix] Intelligent Target Sizing - Ultimate Fix!
            // FastLane only receives TypeA_Sprint or Dedicated Small (<30ms) formats.
            // ALL jobs in FastLane should be decoded at Full Resolution.
            // There are NO large images here that need a 256x256 thumbnail.
            int targetW = 0; 
            int targetH = 0;
            
            // [Unified Logic] SVG uses target=0 like other formats (User Request: Remove 80% special case)

            // [Direct D2D] Load directly to RawImageFrame backed by Arena
            {
                auto* ctx = new(std::nothrow) AuxLayerReadyCtx{ m_parent, cmd.path, cmd.id, cmd.targetSlot, cmd.generationId };
                if (ctx) {
                    rawFrame.onAuxLayerReady.ctx = ctx;
                    rawFrame.onAuxLayerReady.pfn = [](void* c, std::unique_ptr<QuickView::AuxLayer> aux, QuickView::GpuBlendOp op, QuickView::GpuShaderPayload payload) {
                        auto* lctx = static_cast<AuxLayerReadyCtx*>(c);
                        EngineEvent ev;
                        ev.type = EventType::AuxLayerReady;
                        ev.filePath = lctx->path;
                        ev.imageId = lctx->id;
                        ev.targetSlot = lctx->targetSlot;
                        ev.generationId = lctx->generationId;
                        ev.auxLayer = std::move(aux);
                        ev.blendOp = op;
                        ev.shaderPayload = payload;
                        lctx->engine->QueueEvent(std::move(ev));
                    };
                    rawFrame.onAuxLayerReady.ctxDeleter = [](void* c) { delete static_cast<AuxLayerReadyCtx*>(c); };
                }
            }
            HRESULT hr = m_loader->LoadToFrame(cmd.path.c_str(), &rawFrame, &arena, targetW, targetH, &loaderName, {}, nullptr, true, false, cmd.targetHdrHeadroomStops);
            
            int decodeMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();

            if (SUCCEEDED(hr) && rawFrame.IsValid()) {
                // Determine blurriness
                // If we did a full decode (target=0 or result close to original), it's Clear.
                // Otherwise it's a Thumbnail (Blurry/Preview).
                // [Fix] targetW=0 means full decode requested.
                // FastLane does not output dirty previews anymore.
                bool isClear = true;
                
                // Save Scout Loader Name for HUD
                {
                    std::lock_guard lock(m_debugMutex);
                    m_lastLoaderName = loaderName;
                }
                
                EngineEvent e;
                e.type = isClear ? EventType::FullReady : EventType::PreviewReady;
                e.filePath = cmd.path;
                e.imageId = cmd.id;
                e.targetSlot = cmd.targetSlot;
                e.generationId = cmd.generationId;
                
                // [v8.16 Fix] DEEP COPY pixels to heap BEFORE outputting event!
                // When FastLane immediately starts next job, Arena memory is reused.
                // Main thread may not have consumed this frame yet -> corruption!
                auto safeFrame = std::make_shared<QuickView::RawImageFrame>();
                if (rawFrame.IsSvg()) {
                    safeFrame->format = rawFrame.format;
                    safeFrame->formatDetails = rawFrame.formatDetails;
                    safeFrame->width = rawFrame.width;
                    safeFrame->height = rawFrame.height;
                    safeFrame->svg = std::make_unique<QuickView::RawImageFrame::SvgData>();
                    safeFrame->svg->xmlData = rawFrame.svg->xmlData;
                    safeFrame->svg->viewBoxW = rawFrame.svg->viewBoxW;
                    safeFrame->svg->viewBoxH = rawFrame.svg->viewBoxH;
                } else {
                    size_t bufferSize = rawFrame.GetBufferSize();
                    uint8_t* heapPixels = new uint8_t[bufferSize];
                    memcpy(heapPixels, rawFrame.pixels, bufferSize);
                    
                    safeFrame->pixels = heapPixels;
                    safeFrame->width = rawFrame.width;
                    safeFrame->height = rawFrame.height;
                    safeFrame->stride = rawFrame.stride;
                    safeFrame->format = rawFrame.format;
                    safeFrame->formatDetails = rawFrame.formatDetails;
                    safeFrame->quality = QuickView::DecodeQuality::Preview; 
                    safeFrame->exifOrientation = rawFrame.exifOrientation;
                    safeFrame->memoryDeleter = QuickView::MemoryDeleter::FromDeleteArray();
                    
                    // [CMS] Propagate color profile and HDR metadata
                    safeFrame->iccProfile = std::move(rawFrame.iccProfile);
                    safeFrame->colorInfo = rawFrame.colorInfo;
                    safeFrame->hdrMetadata = rawFrame.hdrMetadata;
                    safeFrame->srcWidth = rawFrame.srcWidth;
                    safeFrame->srcHeight = rawFrame.srcHeight;

                    // [GPU Pipeline] Deep copy blend operation and payload
                    safeFrame->blendOp = rawFrame.blendOp;
                    safeFrame->shaderPayload = rawFrame.shaderPayload;
                    if (rawFrame.auxLayer) safeFrame->auxLayer = rawFrame.auxLayer->Clone();
                    
                    // [v10.5] Animation Decoder propagation
                    QV_LOG("Anim_Probe", TraceLoggingString("Hijacked to Animator", "Action"));
                    safeFrame->animator = rawFrame.animator;
                    safeFrame->frameMeta = rawFrame.frameMeta;
                }
                e.rawFrame = safeFrame;

                e.metadata.Width = rawFrame.width;
                e.metadata.Height = rawFrame.height;
                e.metadata.Format = info.format; // [Scout] Direct from PeekHeader
                e.metadata.FormatDetails = rawFrame.formatDetails;
                e.metadata.LoaderName = loaderName;
                e.metadata.FileSize = info.fileSize;
                e.metadata.colorInfo = rawFrame.colorInfo;
                e.metadata.hdrMetadata = rawFrame.hdrMetadata;
                if (!e.metadata.HasEmbeddedColorProfile.has_value()) {
                    e.metadata.HasEmbeddedColorProfile =
                        rawFrame.colorInfo.hasEmbeddedIcc || !safeFrame->iccProfile.empty();
                }
                    
                // [v5.3] Metadata is now populated by LoadToFrame (Unified path)
                // We don't call LoadToFrame with pMetadata in FastLane currently, 
                // but info.format from PeekHeader is sufficient for Scout mismatch check.
                
                // [Fix] Propagate EXIF Orientation from Decoder to Metadata (Critical for AutoRotate)
                e.metadata.ExifOrientation = rawFrame.exifOrientation;
                
                // [v5.3 Lazy] Reverted Sync ReadMetadata. 
                // Metadata will be populated only when InfoPanel requests it.
                
                // [v10.3.1 Optimized] Removed Eager Histogram Calculation.
                // Navigation latency was multi-second because ComputeHistogramFromFrame 
                // ran synchronously on the FastLane/Heavy thread even when InfoPanel was hidden.
                // Histogram is now triggered via UpdateHistogramAsync (Lazy) in main.cpp.
                /*
                if (e.rawFrame && e.rawFrame->IsValid()) {
                    m_loader->ComputeHistogramFromFrame(*e.rawFrame, &e.metadata);
                }
                */
                
                e.isScaled = !isClear;
                // pixels empty

                // [FIX] Store result in m_results for PollState to retrieve
                // Previously QueueEvent only sent notification but dropped the event!

                
                {
                    std::lock_guard lock(m_queueMutex);
                    m_results.push_back(std::move(e));
                }
                
                // Signal main thread
                m_parent->QueueEvent(EngineEvent{}); // Dummy event, just for notification
                
                if (isClear) QV_LOG("FastLane_Output", TraceLoggingString("FullReady", "Action"));
                else QV_LOG("FastLane_Output", TraceLoggingString("PreviewReady", "Action"), TraceLoggingInt32(targetW, "TargetW"), TraceLoggingInt32(rawFrame.width, "RawW"));
                
                if (isClear) {
                    m_parent->CancelHeavy(cmd.id, cmd.targetSlot);
                }
            } else {


                // [v7.5] Handle Abort/Failure (Modular JXL)
                // Unified Error Path: Push Event so PollState can handle it (Trigger Pending Heavy)
                if (hr == E_ABORT || FAILED(hr)) {
                    EngineEvent e;
                    e.type = EventType::LoadError;
                    e.filePath = cmd.path;
                    e.imageId = cmd.id;
                    e.targetSlot = cmd.targetSlot;
                    e.generationId = cmd.generationId;
                    e.hr = hr; // Propagate error code
                    {
                        std::lock_guard lock(m_queueMutex);
                        m_results.push_back(std::move(e));
                    }
                    m_parent->QueueEvent(EngineEvent{}); // Signal
                }
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            int totalMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            m_lastDecodeTimeMs.store(decodeMs);
            m_lastTotalTimeMs.store(totalMs);
            m_lastLoadId.store(cmd.id);
            
            m_isWorking = false; // [HUD V4] Idle

        }
    }
    QV_LOG("FastLane_Lifecycle", TraceLoggingString("WorkerExiting", "Action"));
}

void ImageEngine::SetPrefetchPolicy(const PrefetchPolicy& policy) {
    m_prefetchPolicy = policy;
}

void ImageEngine::SetGalleryPriorityMode(bool active) {
    if (m_galleryPriorityMode.exchange(
            active, std::memory_order_acq_rel) == active) {
        return;
    }

    // This queue is owned by the UI thread. Let already-running normal-viewer
    // jobs finish so their decoded frames remain reusable by the gallery.
    m_prefetchQueue.clear();
    if (!active && m_navigator) {
        const int currentIndex = m_currentViewIndex.load();
        if (currentIndex >= 0) {
            const int direction = m_lastDirectionInt.load();
            const QuickView::BrowseDirection browseDirection =
                direction < 0 ? QuickView::BrowseDirection::BACKWARD
                : direction > 0 ? QuickView::BrowseDirection::FORWARD
                                : QuickView::BrowseDirection::IDLE;
            UpdateView(currentIndex, browseDirection);
        }
    }
}

void ImageEngine::TriggerPendingJxlHeavy() {
    if (!m_pendingJxlHeavyPath.empty() && m_pendingJxlHeavyId != 0) {
        QV_LOG("PollState_Route", TraceLoggingString("JXL Sequential TriggerHeavy", "Action"));
        m_heavyPool->Submit(m_pendingJxlHeavyPath, m_pendingJxlHeavyId, nullptr, m_pendingJxlHeavySlot, m_pendingJxlHeavyGenerationId);
        m_pendingJxlHeavyPath.clear();
        m_pendingJxlHeavyId = 0;
    }
}

size_t ImageEngine::GetCacheMemoryUsage() const {
    std::lock_guard lock(m_cacheMutex);
    return m_currentCacheBytes;
}

int ImageEngine::GetCacheItemCount() const {
    std::lock_guard lock(m_cacheMutex);
    return (int)m_cache.size();
}

std::shared_ptr<QuickView::RawImageFrame> ImageEngine::GetCachedImage(
    const std::wstring& path,
    CImageLoader::ImageMetadata* metadata) {
    std::lock_guard lock(m_cacheMutex);
    auto it = m_cache.find(path);
    if (it == m_cache.end()) return nullptr;
    if (metadata && it->second.hasMetadata) {
        *metadata = it->second.metadata;
    }
    return it->second.frame;
}

void ImageEngine::UpdateView(int currentIndex, QuickView::BrowseDirection dir) {
    m_currentViewIndex.store(currentIndex);
    // [v8.15] Store direction as int for atomic access
    int dirInt = (dir == QuickView::BrowseDirection::FORWARD) ? 1 : 
                 (dir == QuickView::BrowseDirection::BACKWARD) ? -1 : 0;
    m_lastDirectionInt.store(dirInt);
    
    QV_LOG("Engine_UpdateView",
        TraceLoggingInt32(currentIndex, "Index"),
        TraceLoggingInt32(dirInt, "Direction"));
    
    // Evict stale cache entries, then replace only the desired lookahead
    // window. Already-running background decodes remain useful and complete;
    // visible work enters HeavyLane at a higher priority.
    PruneQueue(currentIndex, dir);
    
    // FullGrid has its own physical-core thumbnail scheduler. Do not admit
    // additional full-frame lookahead while visible cells are unresolved.
    if (m_galleryPriorityMode.load(std::memory_order_acquire) ||
        !m_prefetchPolicy.enablePrefetch) {
        m_prefetchQueue.clear();
        return;
    }

    m_prefetchQueue.clear();

    if (dir == QuickView::BrowseDirection::IDLE) {
        // Warm both directions before the first input. Alternate sides so
        // either navigation key starts on a hot frame.
        const int count = static_cast<int>(m_navigator->Count());
        const int radius = (std::min)(m_prefetchPolicy.lookAheadCount, count - 1);
        for (int distance = 1; distance <= radius; ++distance) {
            const QuickView::Priority priority =
                (distance == 1) ? QuickView::Priority::High : QuickView::Priority::Idle;
            m_prefetchQueue.push_back({(currentIndex + distance) % count, priority});
            m_prefetchQueue.push_back({
                (currentIndex - distance + count) % count, priority});
        }
    } else {
        const int step =
            (dir == QuickView::BrowseDirection::BACKWARD) ? -1 : 1;

        // Fill the physical eight-job budget with the active direction first.
        // The opposite neighbor is useful only after the forward runway.
        for (int distance = 1;
             distance <= m_prefetchPolicy.lookAheadCount;
             ++distance) {
            const QuickView::Priority priority =
                (distance == 1)
                    ? QuickView::Priority::High
                    : QuickView::Priority::Idle;
            m_prefetchQueue.push_back(
                {currentIndex + step * distance, priority});
        }
        m_prefetchQueue.push_back(
            {currentIndex - step, QuickView::Priority::Low});
    }
    
    // Pump queue immediately
    PumpPrefetch();
}

bool ImageEngine::ScheduleJob(int index, QuickView::Priority pri) {
    // 1. Bounds check
    if (!m_navigator) return false;
    if (index < 0 || index >= (int)m_navigator->Count()) return false;
    
    // 2. Get file path
    const std::wstring& path = m_navigator->GetFile(index);
    if (path.empty()) return false;
    
    // 3. Check if already in cache
    {
        std::lock_guard lock(m_cacheMutex);
        if (m_cache.count(path)) return false; // Already cached
    }
    
    // Prefetch jobs may run concurrently with visible navigation. HeavyLane
    // priority keeps the current frame ahead of speculative work.
    
    // [v4.1] Smart Prefetch logic re-enabled (Unified Dispatch Integration)

    // 6. Pre-flight check for classification
    auto info = m_loader->PeekHeader(path.c_str());
    
    // 7. Dispatch based on classification
    uintmax_t fileSize = m_navigator->GetFileSize(index);

    // [v9.4] Smart Skip: If single image > Cache Cap, skip prefetch
    // This prevents "Eco Mode OOM" where a single 90MP image (350MB) 
    // forces overflow despite the 128MB limit.
    // [Titan Fix] If Titan mode is enabled, massive images go to MMF pool (Zero-Copy),
    // so they DON'T consume the same heap cache. We should allow prefetch to MMF!
    std::wstring fmtUpper = info.format;
    std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::toupper);
    const bool isTitanFormat =
        (fmtUpper == L"JPEG" || fmtUpper == L"JPG" ||
         fmtUpper == L"WEBP" || fmtUpper == L"PNG" ||
         fmtUpper == L"JXL" || fmtUpper == L"TIF" ||
         fmtUpper == L"TIFF" || fmtUpper == L"AVIF");
    const bool isTitanCandidate =
        isTitanFormat &&
        ((info.width > 8192 || info.height > 8192) ||
         ((uint64_t)info.width * (uint64_t)info.height > 50000000ULL) ||
         ((info.width <= 0 || info.height <= 0) && fileSize >= (32ull * 1024ull * 1024ull)));

    if (pri != QuickView::Priority::Critical && isTitanCandidate) {
        QV_LOG("Engine_Trace", TraceLoggingString("NonTitan Skip", "Action"));
        return false;
    }

    if (pri != QuickView::Priority::Critical && m_prefetchPolicy.maxCacheMemory > 0 && !IsTitanModeEnabled()) {
         uint64_t predictedSize = (uint64_t)info.width * info.height * 4;
         // Allow a 10% margin just in case, but strictly reject if it consumes > 90% of ENTIRE cache
         if (predictedSize > m_prefetchPolicy.maxCacheMemory) {
              QV_LOG("Prefetch_Skip",
                  TraceLoggingString("OverCacheCap", "Action"),
                  TraceLoggingFloat64(predictedSize / 1048576.0, "PredictedMB"),
                  TraceLoggingFloat64(m_prefetchPolicy.maxCacheMemory / 1048576.0, "CacheCapMB"));
              return false;
         }
    }
    
    // AVIF decode is CPU-heavy even below the generic 4 MP Sprint threshold.
    // A serial FastLane leaves seven prefetched frames queued while HeavyLane
    // sits idle. Spread AVIF lookahead across the bounded HeavyLane pool.
    const bool useParallelPrefetch = (fmtUpper == L"AVIF");
    if (info.type == CImageLoader::ImageType::TypeA_Sprint &&
        !useParallelPrefetch) {
        {
            std::lock_guard lock(m_pendingMutex);
            if (!m_pendingPaths.insert(path).second) return false;
        }
        m_fastLane.Push(
            path,
            ComputePathHash(path),
            m_targetHdrHeadroomStops.load(std::memory_order_relaxed));
        return true;
    } else if (info.type == CImageLoader::ImageType::TypeB_Heavy ||
               useParallelPrefetch) {
        const ImageID pathId = ComputePathHash(path);
        if (m_heavyPool->HasStandardWork(pathId, PaneSlot::Primary)) return false;
        {
            std::lock_guard lock(m_pendingMutex);
            m_pendingPaths.insert(path);
        }

        const int poolPriority =
            (pri == QuickView::Priority::Critical) ? 300 :
            (pri == QuickView::Priority::High) ? 180 :
            (pri == QuickView::Priority::Low) ? 80 : 100;
        if (info.format == L"JXL") {
            m_heavyPool->SubmitFullDecode(
                path, pathId, nullptr,
                PaneSlot::Primary, 0, poolPriority);
        } else {
            m_heavyPool->Submit(
                path, pathId, nullptr,
                PaneSlot::Primary, 0, poolPriority);
        }
        return true;
    }
    return false;
}

void ImageEngine::PruneQueue(int currentIndex, QuickView::BrowseDirection /*dir*/) {
    // Calculate valid range based on direction
    // Remove unused minValid and maxValid
    
    // FastLane already has skip-middle logic
    // Heavy lane has single-slot replacement
    // Cache eviction handles the rest
    EvictCache(currentIndex);
}

void ImageEngine::AddToCache(
    int index,
    const std::wstring& path,
    std::shared_ptr<QuickView::RawImageFrame> frame,
    const CImageLoader::ImageMetadata& metadata) {
    if (!frame || !frame->IsValid()) return;
    
    // [v10.5] Skip caching for animated images - animator is stateful and cannot be deep-copied.
    // Re-decoding through FastLane on re-navigation is cheap for small animated files.
    if (frame->animator) return;
    
    // 1. Calculate true backing-store size.
    // Float HDR frames are 16 Bpp, not 4 Bpp, so width*height*4 badly underestimates
    // both cache pressure and the cost of prefetch deep copies.
    size_t newSize = frame->GetBufferSize();
    
    std::lock_guard lock(m_cacheMutex);
    
    // 2. Check if already cached
    auto it = m_cache.find(path);
    if (it != m_cache.end()) {
        // [v9.0] Smart Upgrade: Allow overwriting Preview with Full
        if (it->second.frame->quality == QuickView::DecodeQuality::Preview && 
            frame->quality == QuickView::DecodeQuality::Full) {
            
            // Allow overwrite!
            // Remove old size from tracker
            m_currentCacheBytes -= it->second.sizeBytes;
            // Continue to overwrite logic...
            
            // Note: Standard map::operator[] below will overwrite.
            // But we need to be careful about LRU order?
            // Existing logic pushes to push_front, but doesn't remove old LRU entry if it exists?
            // Actually AddToCache is implemented as:
            // m_cache[path] = entry; m_lruOrder.push_front(path);
            // If we just proceed, we get duplicate in LRU list (safe but inefficient).
            // Let's remove the old LRU entry to be clean.
            auto lit = std::find(m_lruOrder.begin(), m_lruOrder.end(), path);
            if (lit != m_lruOrder.end()) m_lruOrder.erase(lit);
            
        } else {
             return; // Already cached (Equal or Better quality)
        }
    }
    
    // 3. Memory limit check with eviction
    size_t effectiveCacheLimit = m_prefetchPolicy.maxCacheMemory;

    // Keep a proportional reserve so cache growth responds to actual memory
    // pressure: 20% of physical RAM, never less than 2 GiB.
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        const uint64_t systemReserve = (std::max)(
            2ULL * 1024 * 1024 * 1024,
            memory.ullTotalPhys / 5);
        const uint64_t growableBytes =
            memory.ullAvailPhys > systemReserve
                ? memory.ullAvailPhys - systemReserve
                : 0;
        effectiveCacheLimit = static_cast<size_t>((std::min)(
            static_cast<uint64_t>(effectiveCacheLimit),
            static_cast<uint64_t>(m_currentCacheBytes) + growableBytes));
    }
    while (m_currentCacheBytes + newSize > effectiveCacheLimit &&
           !m_lruOrder.empty()) {
        // Protect current ±1, but continue scanning older entries instead of
        // letting one protected tail item block all future cache turnover.
        auto victimListIt = m_lruOrder.end();
        for (auto candidate = m_lruOrder.end();
             candidate != m_lruOrder.begin();) {
            --candidate;
            const auto cached = m_cache.find(*candidate);
            if (cached == m_cache.end() ||
                abs(cached->second.sourceIndex - m_currentViewIndex) > 1) {
                victimListIt = candidate;
                break;
            }
        }
        if (victimListIt == m_lruOrder.end()) break;

        const auto cached = m_cache.find(*victimListIt);
        if (cached != m_cache.end()) {
            m_currentCacheBytes -= cached->second.sizeBytes;
            m_cache.erase(cached);
        }
        m_lruOrder.erase(victimListIt);
    }
    
    // 4. Add to cache (ProtectionZone allows exceeding limit)
    // Only add if we have space OR if it's high priority (neighbor).
    if (m_currentCacheBytes + newSize <= effectiveCacheLimit || abs(index - m_currentViewIndex) <= 1) {
        std::shared_ptr<QuickView::RawImageFrame> cachedFrame;

        // FastLane and HeavyLane events already own stable heap buffers.
        // Share those frames instead of copying every decoded image a second
        // time. Arena-backed frames have no deleter and still require a copy.
        if (frame->IsSvg() || frame->memoryDeleter) {
            cachedFrame = std::move(frame);
        } else {
            cachedFrame = std::make_shared<QuickView::RawImageFrame>();
            size_t bufferSize = frame->GetBufferSize();
            uint8_t* heapPixels = new uint8_t[bufferSize];
            memcpy(heapPixels, frame->pixels, bufferSize);

            cachedFrame->pixels = heapPixels;
            cachedFrame->width = frame->width;
            cachedFrame->height = frame->height;
            cachedFrame->stride = frame->stride;
            cachedFrame->format = frame->format;
            cachedFrame->quality = frame->quality;
            cachedFrame->formatDetails = frame->formatDetails;
            cachedFrame->exifOrientation = frame->exifOrientation;
            cachedFrame->srcWidth = frame->srcWidth;
            cachedFrame->srcHeight = frame->srcHeight;
            cachedFrame->memoryDeleter = QuickView::MemoryDeleter::FromDeleteArray();
            cachedFrame->iccProfile = frame->iccProfile;
            cachedFrame->colorInfo = frame->colorInfo;
            cachedFrame->hdrMetadata = frame->hdrMetadata;
            cachedFrame->blendOp = frame->blendOp;
            cachedFrame->shaderPayload = frame->shaderPayload;
            if (frame->auxLayer) cachedFrame->auxLayer = frame->auxLayer->Clone();
        }

        CacheEntry entry;
        entry.frame = std::move(cachedFrame);
        entry.sourceIndex = index;
        entry.sizeBytes = newSize;
        entry.metadata = metadata;
        entry.hasMetadata = true;

        m_cache[path] = std::move(entry);
        m_lruOrder.push_front(path);
        m_currentCacheBytes += newSize;
    }
}

void ImageEngine::EvictCache(int currentIndex) {
    std::lock_guard lock(m_cacheMutex);
    
    // Evict entries far from current view
    auto it = m_lruOrder.begin();
    while (it != m_lruOrder.end()) {
        auto cit = m_cache.find(*it);
        if (cit != m_cache.end()) {
            int idx = cit->second.sourceIndex;
            
            // Keep if within ± lookAheadCount
            if (abs(idx - currentIndex) > m_prefetchPolicy.lookAheadCount + 1) {
                m_currentCacheBytes -= cit->second.sizeBytes;
                m_cache.erase(cit);
                it = m_lruOrder.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// [v5.3] Async Request for Auxiliary Metadata (EXIF/Stats)
void ImageEngine::RequestFullMetadata() {
    // Capture current state safely
    std::wstring path = m_currentNavPath;
    ImageID id = m_currentImageId;
    
    if (path.empty()) return;

    // [v5.3] Debounce Logic
    {
        std::lock_guard<std::mutex> lock(m_fastLane.m_pendingMutex);
        if (m_fastLane.m_pendingMetadataRequests.count(id)) return; // Already requested
        m_fastLane.m_pendingMetadataRequests.insert(id);
    }

    // Launch Async (Detached)
    std::thread([this, path, id]() {
        // [v5.4] Robustness: RAII Cleaner to ensure we ALWAYS remove from pending set
        struct PendingCleaner {
            FastLane& lane;
            ImageID id;
            ~PendingCleaner() {
                std::lock_guard<std::mutex> lock(lane.m_pendingMutex);
                lane.m_pendingMetadataRequests.erase(id);
            }
        } cleaner{m_fastLane, id};

        {
            // Initialize COM for WIC
            HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
            
            CImageLoader tempLoader; 

            // [v5.3 Fix] Create local WIC Factory for temp loader
            Microsoft::WRL::ComPtr<IWICImagingFactory> pFactory;
            bool factoryOk = false;
            
            if (SUCCEEDED(hr)) {
                 HRESULT hrFactory = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
                 if (SUCCEEDED(hrFactory)) {
                     tempLoader.Initialize(pFactory.Get());
                     factoryOk = true;
                 } else {
                     QV_LOG("Engine_AsyncMeta", TraceLoggingString("WIC Factory Failed", "Action"));
                 }
            }
            
            if (factoryOk) {
                CImageLoader::ImageMetadata meta;
                
                // Pass 'clear=true' to ensure fresh struct
                tempLoader.ReadMetadata(path.c_str(), &meta, true);
                
                EngineEvent evt;
                evt.type = EventType::MetadataReady;
                evt.imageId = id;
                evt.filePath = path; 
                evt.metadata = std::move(meta);
                
                // Inject into Scout Results Queue (Thread Safe)
                {
                    std::lock_guard<std::mutex> lock(m_fastLane.m_queueMutex);
                    m_fastLane.m_results.push_back(std::move(evt));
                }
                
                // Signal Main Thread (via dummy event)
                // [v5.5 Fix] MUST wake up the message loop, otherwise MetadataReady is ignored until user input!
                QueueEvent(EngineEvent{}); 
            }
            
            if (SUCCEEDED(hr)) CoUninitialize();
            
        }
        
        // Destructor of 'cleaner' runs here, removing ID from pending set.
        
    }).detach();
}



// Continuously feed the worker pools instead of waiting for total engine idle.
// Capacity comes from actual queue/worker/result state, not the HUD's
// pending-path set: cancelled decodes can legitimately leave a stale display
// marker, but must never stop future lookahead work.
void ImageEngine::PumpPrefetch() {
    if (m_galleryPriorityMode.load(std::memory_order_acquire) ||
        m_prefetchQueue.empty() || !m_navigator) {
        return;
    }

    constexpr int maxSpeculativeInFlight = 8;
    int speculativeInFlight = m_heavyPool->GetStandardWorkCount();
    speculativeInFlight += static_cast<int>(m_fastLane.GetQueueSize());
    if (m_fastLane.m_isWorking.load(std::memory_order_relaxed)) {
        ++speculativeInFlight;
    }

    // Header classification can parse container boxes. Bound it per UI turn;
    // later PollState ticks continue filling the same physical work window.
    constexpr int maxClassificationsPerPump = 2;
    int classifications = 0;
    while (!m_prefetchQueue.empty() &&
           speculativeInFlight < maxSpeculativeInFlight &&
           classifications < maxClassificationsPerPump) {
        const auto task = m_prefetchQueue.front();
        m_prefetchQueue.pop_front();
        if (task.index < 0 ||
            task.index >= static_cast<int>(m_navigator->Count())) {
            continue;
        }

        const std::wstring& path = m_navigator->GetFile(task.index);
        {
            std::lock_guard lock(m_cacheMutex);
            if (m_cache.count(path)) continue;
        }

        ++classifications;
        if (ScheduleJob(task.index, task.priority)) {
            ++speculativeInFlight;
        }
    }
}

// [Fix] Invalidate specific cache entry (e.g. after Edit/Save)
void ImageEngine::InvalidateCache(const std::wstring& path) {
    std::lock_guard lock(m_cacheMutex);
    
    auto cit = m_cache.find(path);
    if (cit != m_cache.end()) {
        m_currentCacheBytes -= cit->second.sizeBytes;
        m_cache.erase(cit);
        
        // Remove from LRU list (O(N) unfortunately, but safe)
        // Finding element in list by value needs scan
        auto lit = std::find(m_lruOrder.begin(), m_lruOrder.end(), path);
        if (lit != m_lruOrder.end()) {
            m_lruOrder.erase(lit);
        }
    }
}

bool ImageEngine::IsTitanModeEnabled() const {
    return m_heavyPool && m_heavyPool->IsTitanMode();
}

void ImageEngine::UpdateTileViewport(QuickView::RegionRect viewport, float scale, int imageW, int imageH, float basePreviewRatio, float velocityX, float velocityY) {
    if (!m_heavyPool) return;
    
    // [Infinity Engine] Update Manager & Get Missing
    // Pass image dimensions and preview ratio for clamping and triggering
    std::vector<QuickView::TileKey> missing = m_tileManager->Update(viewport, scale, velocityX, velocityY, imageW, imageH, basePreviewRatio);
    
    if (missing.empty()) return;
    
    // Batch Submit
    std::vector<HeavyLanePool::TilePriorityRequest> batch;

    for (const auto& key : missing) {
        // Convert TileKey -> TileCoord (Legacy)
        QuickView::TileCoord coord;
        coord.col = key.x();
        coord.row = key.y();
        coord.lod = key.level();
        
        // Calculate Region (Image Space)
        int tileSize = QuickView::TILE_SIZE << key.level();
        QuickView::RegionRect srcRect;
        srcRect.x = key.x() * tileSize;
        srcRect.y = key.y() * tileSize;
        srcRect.w = tileSize;
        srcRect.h = tileSize;

        // Setup Request
        QuickView::RegionRequest req;
        req.srcRect = srcRect;
        req.dstWidth = QuickView::TILE_SIZE;
        req.dstHeight = QuickView::TILE_SIZE;
        
        // [Fix Spiral Priority] Calculate -Distance priority
        // Prioritize Center -> Outwards
        float cx = viewport.x + viewport.w * 0.5f;
        float cy = viewport.y + viewport.h * 0.5f;
        float tcx = srcRect.x + srcRect.w * 0.5f;
        float tcy = srcRect.y + srcRect.h * 0.5f;
        float distSq = (cx - tcx)*(cx - tcx) + (cy - tcy)*(cy - tcy);
        
        // Invert distance so closest (smallest dist) has highest (least negative) priority
        // Use squared distance to avoid sqrt, it preserves order.
        int priority = -(int)distSq;
        
        // [Aggressive Caching] Penalty for Off-Screen Tiles
        // Ensure Preloading Ring handles Lower Priority than visible tiles.
        // Check intersection
        bool isInside = (srcRect.x < viewport.x + viewport.w && srcRect.x + srcRect.w > viewport.x &&
                         srcRect.y < viewport.y + viewport.h && srcRect.y + srcRect.h > viewport.y);
        
        if (!isInside) {
            // [Titan Guard] Padding Logic
            // If padding is disabled, SKIP off-screen tiles entirely.
            if (!m_enablePadding) {
                continue; 
            }

            // Apply massive penalty (100M) to ensure strictly lower than any visible tile.
            // Visible tiles will be e.g. -0 to -64M (8K image).
            // So -100M puts it safely behind.
            // Ensure we don't underflow int32.
            // 8K sq = 64,000,000. -2B is min int. We have room.
            priority -= 100000000;
        }

        batch.push_back({ coord, req, priority });
    }
    
    // Use Priority Batch Submission
    if (!batch.empty()) {
        m_heavyPool->SubmitPriorityTileBatch(m_currentNavPath, m_currentImageId.load(), m_mmf, batch);
    }
}

void ImageEngine::InvalidateGpuTiles() {
    if (m_tileManager) m_tileManager->InvalidateGpuTiles();
}



