#include "xvr/VideoEncoder.h"

#include "xvr/Check.h"
#include "xvr/Log.h"

#include <codecapi.h>
#include <icodecapi.h> // ICodecAPI itself; codecapi.h only defines the property GUIDs
#include <mferror.h>
#include <mfapi.h>

#include <format>
#include <mutex>

using winrt::com_ptr;

namespace xvr {
namespace {

constexpr int64_t kHnsPerSecond = 10'000'000;

// Media Foundation must be started before any of its objects are created, and the
// framework cannot assume the host application has done it. Reference counted so that
// several encoder instances can come and go without tearing the platform out from
// underneath each other.
std::mutex g_platformMutex;
int g_platformRefs = 0;

void AcquireMediaFoundation()
{
    std::lock_guard<std::mutex> lock(g_platformMutex);
    if (g_platformRefs == 0)
    {
        const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(hr))
        {
            LogError(L"MFStartup failed: 0x{:08X}", static_cast<uint32_t>(hr));
            XVR_CHECK(hr);
        }
    }
    ++g_platformRefs;
}

void ReleaseMediaFoundation()
{
    std::lock_guard<std::mutex> lock(g_platformMutex);
    if (g_platformRefs > 0 && --g_platformRefs == 0)
    {
        ::MFShutdown();
    }
}

struct EncoderChoice
{
    com_ptr<IMFActivate> activate;
    com_ptr<IMFTransform> transform;
    std::wstring name;
};

// Finds a usable hardware H.264 encoder and returns it *already activated*.
//
// The transform is deliberately created exactly once and kept. Activating an
// IMFActivate, shutting it down, and then activating the same object again crashes the
// Xbox encoder MFT outright - so candidates that fail are discarded, and the one that
// works is never torn down and recreated.
EncoderChoice FindHardwareEncoder()
{
    MFT_REGISTER_TYPE_INFO outputInfo{ MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    XVR_CHECK(::MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                          MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                          &outputInfo, &activates, &count));

    EncoderChoice choice;
    for (UINT32 i = 0; i < count; ++i)
    {
        // Index 0 is not safe to assume usable: a system can advertise an encoder whose
        // hardware is not actually present, and activation fails.
        if (!choice.transform)
        {
            // Read the name before activating - it is an attribute of the activation
            // object and stays available whether or not activation succeeds.
            WCHAR* friendly = nullptr;
            UINT32 length = 0;
            std::wstring candidateName = L"(unnamed)";
            if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly,
                                                           &length)) &&
                friendly)
            {
                candidateName = friendly;
                ::CoTaskMemFree(friendly);
            }

            com_ptr<IMFTransform> transform;
            const HRESULT hr = activates[i]->ActivateObject(IID_PPV_ARGS(transform.put()));
            if (SUCCEEDED(hr))
            {
                choice.activate.copy_from(activates[i]);
                choice.transform = transform;
                choice.name = candidateName;
            }
            else
            {
                LogWarn(L"Encoder '{}' will not activate: 0x{:08X}", candidateName,
                        static_cast<uint32_t>(hr));
                activates[i]->DetachObject();
            }
        }
        activates[i]->Release();
    }

    if (activates)
    {
        ::CoTaskMemFree(activates);
    }

    return choice;
}

com_ptr<IMFSample> WrapTexture(ID3D11Texture2D* texture, int64_t timestampHns, int64_t durationHns)
{
    com_ptr<IMFMediaBuffer> buffer;
    XVR_CHECK(::MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE,
                                          buffer.put()));

    // A DXGI surface buffer starts with zero length; the encoder needs it set.
    com_ptr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(buffer2d.put()))))
    {
        DWORD length = 0;
        if (SUCCEEDED(buffer2d->GetContiguousLength(&length)))
        {
            buffer->SetCurrentLength(length);
        }
    }

    com_ptr<IMFSample> sample;
    XVR_CHECK(::MFCreateSample(sample.put()));
    XVR_CHECK(sample->AddBuffer(buffer.get()));
    XVR_CHECK(sample->SetSampleTime(timestampHns));
    XVR_CHECK(sample->SetSampleDuration(durationHns));
    return sample;
}

} // namespace

void VideoEncoder::Initialize(ID3D11Device* device, const FrameConfig& config,
                              IEncodedFrameSink* sink)
{
    m_config = config;
    m_sink = sink;
    m_frameDurationHns = kHnsPerSecond / static_cast<int64_t>(config.frameRate);

    // Validate before touching the driver. One out-of-range configuration crashes the
    // encoder outright rather than returning an error, so this check is load-bearing.
    std::wstring reason;
    if (!config.Validate(reason))
    {
        LogError(L"Rejected encoder configuration: {}", reason);
        throw HresultException(E_INVALIDARG, "invalid encoder configuration");
    }

    // "macroblocks", spelled out: MB against a bitrate reads as megabytes, and 8160 of those
    // per frame is alarming enough to send someone chasing a memory bug that is not there.
    LogInfo(L"Encoder config: {}x{} @{}fps, {} Mbps ({} macroblocks/frame, {} macroblocks/s)",
            config.width, config.height, config.frameRate, config.bitrateBps / 1'000'000,
            config.MacroblocksPerFrame(), config.MacroblocksPerSecond());

    AcquireMediaFoundation();
    m_platformHeld = true;

    LogInfo(L"Enumerating hardware H.264 encoders");
    EncoderChoice choice = FindHardwareEncoder();
    if (!choice.transform)
    {
        LogError(L"No usable hardware H.264 encoder found");
        throw HresultException(E_FAIL, "no hardware H.264 encoder");
    }

    m_activate = choice.activate;
    m_transform = choice.transform;
    m_name = choice.name;
    LogInfo(L"Activated encoder: {}", m_name);

    com_ptr<IMFAttributes> attributes;
    m_transform->GetAttributes(attributes.put());

    UINT32 asyncFlag = 0;
    UINT32 d3dAware = 0;
    if (attributes)
    {
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asyncFlag);
        attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
        if (asyncFlag)
        {
            // An async MFT refuses to work until explicitly unlocked.
            XVR_CHECK(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
        }
    }
    m_async = asyncFlag != 0;

    LogInfo(L"Encoder flags: async={}, d3d11Aware={}", m_async, d3dAware != 0);

    if (d3dAware)
    {
        UINT token = 0;
        XVR_CHECK(::MFCreateDXGIDeviceManager(&token, m_deviceManager.put()));
        XVR_CHECK(m_deviceManager->ResetDevice(device, token));
        XVR_CHECK(m_transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                              reinterpret_cast<ULONG_PTR>(m_deviceManager.get())));
        LogInfo(L"D3D manager set");
    }

    // Output type must be set before input type on H.264 encoder MFTs.
    com_ptr<IMFMediaType> outputType;
    XVR_CHECK(::MFCreateMediaType(outputType.put()));
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, config.bitrateBps);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    ::MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, config.width, config.height);
    ::MFSetAttributeRatio(outputType.get(), MF_MT_FRAME_RATE, config.frameRate, 1);
    ::MFSetAttributeRatio(outputType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    XVR_CHECK(m_transform->SetOutputType(0, outputType.get(), 0));
    LogInfo(L"Output type set");

    com_ptr<IMFMediaType> inputType;
    XVR_CHECK(::MFCreateMediaType(inputType.put()));
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(inputType.get(), MF_MT_FRAME_SIZE, config.width, config.height);
    ::MFSetAttributeRatio(inputType.get(), MF_MT_FRAME_RATE, config.frameRate, 1);
    ::MFSetAttributeRatio(inputType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    XVR_CHECK(m_transform->SetInputType(0, inputType.get(), 0));
    LogInfo(L"Input type set (NV12)");

    // Ask the MFT what it wants its input surfaces to look like, rather than assuming.
    // These attributes are how a D3D11-aware transform advertises the bind flags, usage
    // and sharing mode its allocator would have used.
    {
        com_ptr<IMFAttributes> inputAttributes;
        if (SUCCEEDED(m_transform->GetInputStreamAttributes(0, inputAttributes.put())) &&
            inputAttributes)
        {
            const auto report = [&inputAttributes](const GUID& key, const wchar_t* label) {
                UINT32 value = 0;
                if (SUCCEEDED(inputAttributes->GetUINT32(key, &value)))
                {
                    LogInfo(L"  input stream {}: {} (0x{:X})", label, value, value);
                }
                else
                {
                    LogInfo(L"  input stream {}: <not set>", label);
                }
            };

            LogInfo(L"Encoder input surface requirements:");
            report(MF_SA_D3D11_BINDFLAGS, L"bind flags");
            report(MF_SA_D3D11_USAGE, L"usage");
            report(MF_SA_D3D11_SHARED, L"shared");
            report(MF_SA_D3D11_SHARED_WITHOUT_MUTEX, L"shared without mutex");
            report(MF_SA_BUFFERS_PER_SAMPLE, L"buffers per sample");
        }
        else
        {
            LogInfo(L"Encoder exposes no input stream attributes");
        }
    }

    ProbeCodecCapabilities();
    ConfigureCodec();

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    if (SUCCEEDED(m_transform->GetOutputStreamInfo(0, &streamInfo)))
    {
        m_providesSamples =
            (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                   MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    }

    if (m_async)
    {
        XVR_CHECK(m_transform->QueryInterface(IID_PPV_ARGS(m_events.put())));
    }

    LogInfo(L"Codec configured; starting stream");
    XVR_CHECK(m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
    XVR_CHECK(m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    XVR_CHECK(m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

    // Started only after streaming begins, so the thread cannot race the transform's setup.
    if (m_async && m_events)
    {
        m_eventThreadRunning.store(true);
        m_eventThread = std::thread(&VideoEncoder::EventThread, this);
    }

    LogInfo(L"Encoder ready: {} ({}, {} samples)", m_name, m_async ? L"async" : L"sync",
            m_providesSamples ? L"provides" : L"caller-allocated");
}

void VideoEncoder::ProbeCodecCapabilities()
{
    com_ptr<ICodecAPI> codec;
    if (FAILED(m_transform->QueryInterface(IID_PPV_ARGS(codec.put()))))
    {
        return;
    }

    // Xbox rejects the AVLowLatencyMode preset outright, so the individual controls that
    // preset would have flipped are what matter. Ask which of them exist rather than
    // assuming: guessing at encoder capabilities has been wrong more than once.
    const struct
    {
        const GUID& option;
        const wchar_t* label;
    } options[] = {
        { CODECAPI_AVLowLatencyMode, L"AVLowLatencyMode" },
        { CODECAPI_AVEncCommonRateControlMode, L"RateControlMode" },
        { CODECAPI_AVEncCommonMeanBitRate, L"MeanBitRate" },
        { CODECAPI_AVEncCommonBufferSize, L"BufferSize (VBV)" },
        { CODECAPI_AVEncCommonQualityVsSpeed, L"QualityVsSpeed" },
        { CODECAPI_AVEncMPVGOPSize, L"GOPSize" },
        { CODECAPI_AVEncMPVDefaultBPictureCount, L"BPictureCount" },
        { CODECAPI_AVEncVideoMaxNumRefFrame, L"MaxNumRefFrame" },
        { CODECAPI_AVEncVideoForceKeyFrame, L"ForceKeyFrame" },
        { CODECAPI_AVEncSliceControlMode, L"SliceControlMode" },
        { CODECAPI_AVEncSliceControlSize, L"SliceControlSize" },
        { CODECAPI_AVEncH264CABACEnable, L"CABAC" },
    };

    LogInfo(L"Codec controls available:");
    for (const auto& entry : options)
    {
        const bool supported = codec->IsSupported(&entry.option) == S_OK;
        const bool modifiable = codec->IsModifiable(&entry.option) == S_OK;
        LogInfo(L"  {:<20} supported={} modifiable={}", entry.label, supported, modifiable);
    }
}

void VideoEncoder::RequestKeyframe()
{
    // Just a flag. The actual SetValue has to happen on the thread that drives the
    // transform, and doing it from the network thread would race the encode.
    m_keyframeRequested.store(true, std::memory_order_release);
}

/** Applies a pending keyframe request, if one came in since the last frame. */
void VideoEncoder::SetBitrate(uint32_t bitrateBps)
{
    // Staged, like the keyframe request: SetValue has to happen on the thread driving the
    // transform, and this is called from the frame loop.
    m_pendingBitrateBps.store(bitrateBps, std::memory_order_release);
}

void VideoEncoder::ApplyPendingBitrate()
{
    const uint32_t requested = m_pendingBitrateBps.exchange(0, std::memory_order_acq_rel);
    if (requested == 0 || requested == m_currentBitrateBps)
    {
        return;
    }

    com_ptr<ICodecAPI> codec;
    if (FAILED(m_transform->QueryInterface(IID_PPV_ARGS(codec.put()))))
    {
        return;
    }

    VARIANT variant{};
    variant.vt = VT_UI4;
    variant.ulVal = requested;
    if (FAILED(codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &variant)))
    {
        return;
    }

    // The VBV buffer is sized from the bitrate, so it has to move with it. Left at the old
    // value it would let rate control spend several frames' worth of bits on one frame,
    // which is the burst this is trying to avoid.
    VARIANT vbv{};
    vbv.vt = VT_UI4;
    vbv.ulVal = requested / m_config.frameRate;
    codec->SetValue(&CODECAPI_AVEncCommonBufferSize, &vbv);

    LogInfo(L"Bitrate {} -> {} kbps", m_currentBitrateBps / 1000, requested / 1000);
    m_currentBitrateBps = requested;
}

void VideoEncoder::ApplyPendingKeyframeRequest()
{
    if (!m_keyframeRequested.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    com_ptr<ICodecAPI> codec;
    if (FAILED(m_transform->QueryInterface(IID_PPV_ARGS(codec.put()))))
    {
        return;
    }

    VARIANT variant{};
    variant.vt = VT_UI4;
    variant.ulVal = 1;
    const HRESULT hr = codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &variant);
    if (FAILED(hr))
    {
        LogWarn(L"Could not force a keyframe: 0x{:08X}", static_cast<uint32_t>(hr));
        return;
    }
    m_keyframesForced++;
}

void VideoEncoder::ConfigureCodec()
{
    com_ptr<ICodecAPI> codec;
    if (FAILED(m_transform->QueryInterface(IID_PPV_ARGS(codec.put()))))
    {
        LogWarn(L"ICodecAPI unavailable; using encoder defaults");
        return;
    }

    const auto trySetU32 = [&codec](const GUID& option, uint32_t value, const wchar_t* label) {
        VARIANT variant{};
        variant.vt = VT_UI4;
        variant.ulVal = value;
        const HRESULT hr = codec->SetValue(&option, &variant);
        if (SUCCEEDED(hr))
        {
            LogInfo(L"  set {} = {}", label, value);
        }
        else
        {
            LogWarn(L"  could not set {}: 0x{:08X}", label, static_cast<uint32_t>(hr));
        }
        return SUCCEEDED(hr);
    };

    LogInfo(L"Applying low-latency profile:");

    // The preset, if it happens to be available. Everything below is what it would have
    // done anyway, applied individually so the profile still works where it is not.
    {
        VARIANT lowLatency{};
        lowLatency.vt = VT_BOOL;
        lowLatency.boolVal = VARIANT_TRUE;
        if (SUCCEEDED(codec->SetValue(&CODECAPI_AVLowLatencyMode, &lowLatency)))
        {
            LogInfo(L"  set AVLowLatencyMode = true");
        }
    }

    // The single biggest latency win. A B-frame is predicted from a *later* frame, so the
    // encoder must hold it back until that frame arrives - directly adding frames of
    // delay. There is no acceptable amount of this in a live VR stream.
    trySetU32(CODECAPI_AVEncMPVDefaultBPictureCount, 0, L"BPictureCount");

    // One reference frame: less internal buffering, and a lost frame cannot poison a long
    // chain of dependents.
    trySetU32(CODECAPI_AVEncVideoMaxNumRefFrame, 1, L"MaxNumRefFrame");

    // CBR avoids the lookahead that variable rate control needs in order to decide how to
    // spend its bit budget.
    trySetU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR,
              L"RateControlMode(CBR)");
    trySetU32(CODECAPI_AVEncCommonMeanBitRate, m_config.bitrateBps, L"MeanBitRate");
    m_currentBitrateBps = m_config.bitrateBps;

    // A VBV buffer of roughly one frame stops rate control from smoothing bits across
    // many frames, which is exactly the kind of buffering that costs latency.
    trySetU32(CODECAPI_AVEncCommonBufferSize, m_config.bitrateBps / m_config.frameRate,
              L"BufferSize");

    // Bias toward speed: encode time is latency here, not a throughput statistic.
    trySetU32(CODECAPI_AVEncCommonQualityVsSpeed, 20, L"QualityVsSpeed");

    // A long GOP saves bandwidth but makes packet loss catastrophic and recovery slow.
    // One keyframe per second is a starting point; revisit once the network layer exists
    // and real loss behaviour can be measured.
    trySetU32(CODECAPI_AVEncMPVGOPSize, m_config.frameRate, L"GOPSize");
}

void VideoEncoder::ResetLatencyStats()
{
    m_latencyTotalMs = 0.0;
    m_latencyMinMs = 0.0;
    m_latencyMaxMs = 0.0;
    m_latencySamples = 0;
}

VideoEncoder::LatencyStats VideoEncoder::GetLatencyStats() const
{
    LatencyStats stats;
    stats.samples = m_latencySamples;
    if (m_latencySamples > 0)
    {
        stats.minMs = m_latencyMinMs;
        stats.maxMs = m_latencyMaxMs;
        stats.avgMs = m_latencyTotalMs / static_cast<double>(m_latencySamples);
    }
    return stats;
}

void VideoEncoder::Submit(ID3D11Texture2D* nv12Texture, int64_t timestampHns, uint32_t frameIndex,
                          uint64_t captureTimeUs, uint32_t poseSequence)
{
    // Before the frame is wrapped, so the request applies to this frame rather than the
    // next one. Cheap when nothing is pending - one relaxed exchange.
    ApplyPendingKeyframeRequest();
    ApplyPendingBitrate();

    com_ptr<IMFSample> sample = WrapTexture(nv12Texture, timestampHns, m_frameDurationHns);

    {
        // The event thread reads this map when output arrives, so writes are guarded.
        std::lock_guard<std::mutex> lock(m_submitMutex);
        m_submitTimes[timestampHns] = SubmitRecord{ std::chrono::steady_clock::now(), frameIndex,
                                                    captureTimeUs, poseSequence };

    // Bounded deliberately. Entries are normally erased when their output comes back, but
    // any frame the encoder drops - or whose timestamp it alters - leaves one behind
    // forever. Over a long session that grows without limit and slows every lookup, which
    // shows up as a stream that degrades the longer it runs rather than failing outright.
        constexpr size_t kMaxTrackedFrames = 128;
        while (m_submitTimes.size() > kMaxTrackedFrames)
        {
            m_submitTimes.erase(m_submitTimes.begin());
        }
    }

    if (m_async)
    {
        // Output is dispatched by the event thread as soon as it is ready, so this only
        // has to wait for permission to submit.
        {
            std::unique_lock<std::mutex> lock(m_creditMutex);
            if (m_needInputCredits <= 0)
            {
                lock.unlock();
                AwaitNeedInput();
                lock.lock();
            }

            if (m_needInputCredits <= 0)
            {
                // The encoder never asked for this frame. Dropping it is correct: queueing
                // behind a stalled encoder would delay every frame after it too.
                ++m_stats.framesDropped;
                return;
            }
            --m_needInputCredits;
        }

        XVR_CHECK(m_transform->ProcessInput(0, sample.get(), 0));
    }
    else
    {
        XVR_CHECK(m_transform->ProcessInput(0, sample.get(), 0));
        while (ProcessOneOutput())
        {
        }
    }

    ++m_stats.framesSubmitted;
}

void VideoEncoder::EventThread()
{
    while (m_eventThreadRunning.load())
    {
        com_ptr<IMFMediaEvent> event;

        // Blocking wait, which is the point: the encoder wakes this thread the instant a
        // frame is ready rather than the frame waiting for the caller to return.
        const HRESULT hr = m_events->GetEvent(0, event.put());
        if (FAILED(hr))
        {
            // Shutdown closes the event queue, which surfaces here as a failure.
            return;
        }

        MediaEventType type = MEUnknown;
        if (FAILED(event->GetType(&type)))
        {
            continue;
        }

        if (type == METransformNeedInput)
        {
            {
                std::lock_guard<std::mutex> lock(m_creditMutex);
                ++m_needInputCredits;
            }
            m_creditSignal.notify_one();
        }
        else if (type == METransformHaveOutput)
        {
            ProcessOneOutput();
        }
        else if (type == METransformDrainComplete)
        {
            {
                std::lock_guard<std::mutex> lock(m_creditMutex);
                m_drainComplete = true;
            }
            m_creditSignal.notify_all();
        }
    }
}

void VideoEncoder::AwaitNeedInput()
{
    std::unique_lock<std::mutex> lock(m_creditMutex);
    // Bounded: if the encoder stops asking for input, blocking forever would wedge the
    // whole render loop rather than dropping one frame.
    m_creditSignal.wait_for(lock, std::chrono::milliseconds(100),
                            [this] { return m_needInputCredits > 0; });
}

bool VideoEncoder::ProcessOneOutput()
{
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;

    com_ptr<IMFSample> allocated;
    if (!m_providesSamples)
    {
        MFT_OUTPUT_STREAM_INFO info{};
        XVR_CHECK(m_transform->GetOutputStreamInfo(0, &info));

        com_ptr<IMFMediaBuffer> buffer;
        XVR_CHECK(::MFCreateMemoryBuffer(info.cbSize, buffer.put()));
        XVR_CHECK(::MFCreateSample(allocated.put()));
        XVR_CHECK(allocated->AddBuffer(buffer.get()));
        output.pSample = allocated.get();
    }

    DWORD status = 0;
    const HRESULT hr = m_transform->ProcessOutput(0, 1, &output, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        return false;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
    {
        // The encoder renegotiated its output type; accept it and continue.
        com_ptr<IMFMediaType> newType;
        if (SUCCEEDED(m_transform->GetOutputAvailableType(0, 0, newType.put())))
        {
            m_transform->SetOutputType(0, newType.get(), 0);
        }
        return true;
    }

    // The MFT hands back a sample it owns when it provides them; release it either way.
    com_ptr<IMFSample> sample;
    sample.attach(output.pSample);
    if (output.pEvents)
    {
        output.pEvents->Release();
    }

    if (FAILED(hr) || !sample)
    {
        if (FAILED(hr))
        {
            LogError(L"ProcessOutput failed: 0x{:08X}", static_cast<uint32_t>(hr));
        }
        return false;
    }

    // `allocated` and `sample` may be the same object; releasing twice would be a bug.
    if (allocated && allocated.get() == sample.get())
    {
        allocated.detach();
    }

    com_ptr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())))
    {
        return true;
    }

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    if (SUCCEEDED(buffer->Lock(&data, &maxLength, &currentLength)))
    {
        LONGLONG timestamp = 0;
        sample->GetSampleTime(&timestamp);

        UINT32 cleanPoint = 0;
        sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);

        uint32_t frameIndex = 0;
        uint64_t captureTimeUs = 0;
        uint32_t poseSequence = 0;

        // Match this output back to the frame that produced it. Anything older is
        // dropped rather than left to accumulate, so the map cannot grow unbounded if
        // the encoder ever discards a frame.
        //
        // Runs on the event thread while the caller may be submitting, hence the lock.
        std::lock_guard<std::mutex> submitLock(m_submitMutex);
        if (const auto it = m_submitTimes.find(timestamp); it != m_submitTimes.end())
        {
            frameIndex = it->second.frameIndex;
            captureTimeUs = it->second.captureTimeUs;
            poseSequence = it->second.poseSequence;

            const double elapsedMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          it->second.submittedAt)
                    .count();

            if (m_latencySamples == 0 || elapsedMs < m_latencyMinMs)
            {
                m_latencyMinMs = elapsedMs;
            }
            if (elapsedMs > m_latencyMaxMs)
            {
                m_latencyMaxMs = elapsedMs;
            }
            m_latencyTotalMs += elapsedMs;
            ++m_latencySamples;

            m_submitTimes.erase(m_submitTimes.begin(), std::next(it));
        }

        if (m_sink)
        {
            EncodedFrame frame;
            frame.data = data;
            frame.size = currentLength;
            frame.timestampHns = timestamp;
            frame.keyframe = cleanPoint != 0;
            frame.frameIndex = frameIndex;
            frame.captureTimeUs = captureTimeUs;
            frame.poseSequence = poseSequence;
            frame.eye = m_eye;
            m_sink->OnEncodedFrame(frame);
        }

        ++m_stats.framesEncoded;
        m_stats.bytesEncoded += currentLength;

        buffer->Unlock();
    }

    return true;
}

// DeliverPendingOutput is gone: the event thread now dispatches output the moment it is
// ready, so there is nothing left for the caller to pump.

void VideoEncoder::Drain()
{
    if (!m_transform)
    {
        return;
    }

    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);

    if (!m_async)
    {
        while (ProcessOneOutput())
        {
        }
        return;
    }

    // The event thread owns the queue, so waiting here means waiting for it to report the
    // drain rather than reading events directly - two readers would race for them.
    std::unique_lock<std::mutex> lock(m_creditMutex);
    m_drainComplete = false;
    m_creditSignal.wait_for(lock, std::chrono::milliseconds(500),
                            [this] { return m_drainComplete; });
}

void VideoEncoder::Shutdown()
{
    // Stopped before the transform goes away: the thread is blocked inside GetEvent, and
    // that call only returns once the event queue is shut down.
    m_eventThreadRunning.store(false);

    if (m_transform)
    {
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }

    if (m_events)
    {
        // Wakes the blocked GetEvent so the thread can observe the stop flag.
        m_events->QueueEvent(MEError, GUID_NULL, S_OK, nullptr);
    }

    if (m_eventThread.joinable())
    {
        m_eventThread.join();
    }

    m_transform = nullptr;
    m_events = nullptr;

    if (m_activate)
    {
        m_activate->ShutdownObject();
        m_activate->DetachObject();
        m_activate = nullptr;
    }

    m_deviceManager = nullptr;

    if (m_platformHeld)
    {
        ReleaseMediaFoundation();
        m_platformHeld = false;
    }
}

} // namespace xvr
