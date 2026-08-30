// The exported OpenXR entry points.
//
// These are the symbols an application would otherwise get from the Khronos loader. Link
// this library *instead of* openxr_loader - linking both is a duplicate symbol error, and
// linking the loader wins silently on some toolchains, which looks like the runtime simply
// not being used.
//
// Anything not implemented returns XR_ERROR_FUNCTION_UNSUPPORTED from xrGetInstanceProcAddr
// rather than being absent, so an application that asks for it fails where it asked instead
// of at link time.

#include "xvr/openxr/XrRuntime.h"

#include "xvr/Log.h"
#include "xvr/Protocol.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <mutex>

using namespace xvr;
using namespace xvr::xr;

namespace {

// --- Call tracing -----------------------------------------------------------------------
//
// A console that hangs takes Device Portal with it, so there is no crash dump to collect -
// the app never faulted, the machine stopped. The flushed log is the only evidence that
// survives, which makes it worth logging entry and exit of every per-frame entry point.
//
// The point is not the timings. It is that if the last line is "-> xrEndFrame" with no
// matching "<- xrEndFrame", the machine died inside our code; if the exit is there, it died
// in the application's, after we handed control back. Those need completely different work
// and nothing else distinguishes them.
//
// Bounded, because at 90fps this is thousands of lines a second and the interesting part is
// always the start.
#ifdef XVR_TRACE
std::atomic<uint32_t> g_traceBudget{ 4000 };

class TraceScope
{
public:
    explicit TraceScope(const wchar_t* name) : m_name(name)
    {
        m_active = g_traceBudget.load(std::memory_order_relaxed) > 0;
        if (m_active)
        {
            g_traceBudget.fetch_sub(1, std::memory_order_relaxed);
            LogInfo(L"-> {}", m_name);
        }
    }
    ~TraceScope()
    {
        if (m_active)
        {
            LogInfo(L"<- {}", m_name);
        }
    }

private:
    const wchar_t* m_name;
    bool m_active = false;
};
#define XVR_TRACE_CALL(name) TraceScope traceScope_(name)
#else
#define XVR_TRACE_CALL(name)
#endif

// There is one system and it is the Quest on the other end of the link.
constexpr XrSystemId kSystemId = 1;
constexpr uint32_t kViewCount = 2;

Instance* AsInstance(XrInstance handle) { return reinterpret_cast<Instance*>(handle); }
Session* AsSession(XrSession handle) { return reinterpret_cast<Session*>(handle); }
Space* AsSpace(XrSpace handle) { return reinterpret_cast<Space*>(handle); }
Swapchain* AsSwapchain(XrSwapchain handle) { return reinterpret_cast<Swapchain*>(handle); }
ActionSet* AsActionSet(XrActionSet handle) { return reinterpret_cast<ActionSet*>(handle); }
Action* AsAction(XrAction handle) { return reinterpret_cast<Action*>(handle); }

/** The two-call idiom every enumerating OpenXR function uses. */
template <typename T>
XrResult FillArray(const std::vector<T>& source, uint32_t capacity, uint32_t* countOutput,
                   T* buffer)
{
    if (countOutput != nullptr)
    {
        *countOutput = static_cast<uint32_t>(source.size());
    }
    if (capacity == 0)
    {
        return XR_SUCCESS;
    }
    if (capacity < source.size())
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::copy(source.begin(), source.end(), buffer);
    return XR_SUCCESS;
}

XrResult FillString(const char* text, uint32_t capacity, uint32_t* countOutput, char* buffer)
{
    const uint32_t needed = static_cast<uint32_t>(std::strlen(text)) + 1;
    if (countOutput != nullptr)
    {
        *countOutput = needed;
    }
    if (capacity == 0)
    {
        return XR_SUCCESS;
    }
    if (capacity < needed)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::memcpy(buffer, text, needed);
    return XR_SUCCESS;
}

void CopyFixedString(char* destination, size_t size, const char* text)
{
    std::snprintf(destination, size, "%s", text);
}

/** Where a reference space's origin sits, in the client's tracking space. */
XrPosef ReferenceSpaceOrigin(const Session& session, XrReferenceSpaceType type,
                             const FrameContext& frame)
{
    switch (type)
    {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        return ToXrPose(frame.headPosition, frame.headOrientation);
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
    case XR_REFERENCE_SPACE_TYPE_STAGE:
    default:
        // The client sends poses in its own local space and the host has no floor
        // measurement of its own, so stage and local are the same origin here. Saying so
        // is better than inventing a height offset the headset would disagree with.
        return IdentityPose();
    }
    (void)session;
}

/** Transform from `space` into the client's tracking space. */
XrPosef SpaceToTracking(const Space& space, const FrameContext& frame)
{
    if (space.isActionSpace)
    {
        const int hand = space.action != nullptr ? 0 : 0;
        (void)hand;
        return IdentityPose();
    }
    return Multiply(ReferenceSpaceOrigin(*space.session, space.referenceType, frame),
                    space.poseInSpace);
}

const ControllerInput* ControllerFor(const FrameContext& frame, int hand)
{
    if (hand < 0 || hand > 1)
    {
        return nullptr;
    }
    return &frame.controllers[hand];
}

/** Which hand an action state query is asking about, given its subaction path. */
int HandForSubaction(const Instance& instance, XrPath subactionPath)
{
    if (subactionPath == XR_NULL_PATH)
    {
        return -1; // any hand; the caller picks whichever is active
    }
    std::string text;
    if (!instance.paths.Lookup(subactionPath, text))
    {
        return -1;
    }
    return HandFromPath(text);
}

bool ButtonDown(const ControllerInput& controller, Action::Binding binding)
{
    switch (binding)
    {
    case Action::Binding::PrimaryClick:
        return (controller.buttons & net::Button_PrimaryClick) != 0;
    case Action::Binding::SecondaryClick:
        return (controller.buttons & net::Button_SecondaryClick) != 0;
    case Action::Binding::MenuClick:
        return (controller.buttons & net::Button_MenuClick) != 0;
    case Action::Binding::ThumbstickClick:
        return (controller.buttons & net::Button_ThumbstickClick) != 0;
    case Action::Binding::Trigger:
        // An application may bind a float control to a boolean action. OpenXR resolves
        // that with a threshold rather than refusing, so the same is done here.
        return controller.trigger > 0.7f;
    case Action::Binding::Squeeze:
        return controller.squeeze > 0.7f;
    default:
        return false;
    }
}

float AxisValue(const ControllerInput& controller, Action::Binding binding)
{
    switch (binding)
    {
    case Action::Binding::Trigger:
        return controller.trigger;
    case Action::Binding::Squeeze:
        return controller.squeeze;
    case Action::Binding::PrimaryClick:
    case Action::Binding::SecondaryClick:
    case Action::Binding::MenuClick:
    case Action::Binding::ThumbstickClick:
        return ButtonDown(controller, binding) ? 1.0f : 0.0f;
    default:
        return 0.0f;
    }
}

/**
 * Resolves an action query to a hand and the control bound on it.
 *
 * Returns false when the action has no binding on the requested hand, which is a normal
 * outcome rather than an error - OpenXR reports it as isActive false.
 */
bool ResolveAction(const Instance& instance, const Action& action, XrPath subactionPath,
                   const FrameContext& frame, int& handOut, Action::Binding& bindingOut)
{
    const int requested = HandForSubaction(instance, subactionPath);
    for (int hand = 0; hand < 2; ++hand)
    {
        if (requested >= 0 && hand != requested)
        {
            continue;
        }
        if (action.bindings[hand] == Action::Binding::None)
        {
            continue;
        }
        if (!frame.controllers[hand].active)
        {
            continue;
        }
        handOut = hand;
        bindingOut = action.bindings[hand];
        return true;
    }
    return false;
}

} // namespace

extern "C" {

// --- Instance ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateApiLayerProperties(uint32_t, uint32_t* countOutput,
                                                             XrApiLayerProperties*)
{
    if (countOutput != nullptr)
    {
        *countOutput = 0;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(
    const char*, uint32_t propertyCapacityInput, uint32_t* propertyCountOutput,
    XrExtensionProperties* properties)
{
    std::vector<XrExtensionProperties> supported;

    XrExtensionProperties d3d11{ XR_TYPE_EXTENSION_PROPERTIES };
    CopyFixedString(d3d11.extensionName, sizeof(d3d11.extensionName),
                    XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    d3d11.extensionVersion = XR_KHR_D3D11_enable_SPEC_VERSION;
    supported.push_back(d3d11);

    return FillArray(supported, propertyCapacityInput, propertyCountOutput, properties);
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* createInfo,
                                                XrInstance* instance)
{
    if (createInfo == nullptr || instance == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i)
    {
        const char* name = createInfo->enabledExtensionNames[i];
        if (std::strcmp(name, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) != 0)
        {
            LogError(L"OpenXR: extension not supported by this runtime");
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    // Start the log here rather than expecting the application to do it. An OpenXR
    // application has no reason to know XVR exists, let alone to initialise its logging, and
    // without this every diagnostic the runtime produces is discarded - including the ones
    // explaining why a session failed to start. Once only, since LogInit truncates.
    static std::once_flag logOnce;
    std::call_once(logOnce, [] { LogInit(L"xvr-openxr.log"); });

    auto* created = new Instance();
    created->applicationName = createInfo->applicationInfo.applicationName;
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i)
    {
        created->enabledExtensions.emplace_back(createInfo->enabledExtensionNames[i]);
    }

    *instance = reinterpret_cast<XrInstance>(created);
    LogInfo(L"OpenXR instance created for an application");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance)
{
    Instance* self = AsInstance(instance);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (self->session)
    {
        SessionShutdown(*self->session);
        self->session.reset();
    }
    delete self;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance instance,
                                                       XrInstanceProperties* properties)
{
    if (AsInstance(instance) == nullptr || properties == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    properties->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    CopyFixedString(properties->runtimeName, sizeof(properties->runtimeName), "XVR (Xbox to Quest)");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData)
{
    Instance* self = AsInstance(instance);
    if (self == nullptr || eventData == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::lock_guard lock(self->eventMutex);
    if (self->events.empty())
    {
        return XR_EVENT_UNAVAILABLE;
    }
    *eventData = self->events.front();
    self->events.erase(self->events.begin());
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrResultToString(XrInstance, XrResult value,
                                                char buffer[XR_MAX_RESULT_STRING_SIZE])
{
    std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_RESULT_%d", static_cast<int>(value));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrStructureTypeToString(XrInstance, XrStructureType value,
                                                       char buffer[XR_MAX_STRUCTURE_NAME_SIZE])
{
    std::snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_TYPE_%d", static_cast<int>(value));
    return XR_SUCCESS;
}

// --- System -----------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo,
                                           XrSystemId* systemId)
{
    if (AsInstance(instance) == nullptr || getInfo == nullptr || systemId == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
    {
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    }
    *systemId = kSystemId;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId,
                                                     XrSystemProperties* properties)
{
    if (AsInstance(instance) == nullptr || properties == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    properties->systemId = kSystemId;
    properties->vendorId = 0;
    CopyFixedString(properties->systemName, sizeof(properties->systemName), "XVR streamed headset");

    properties->graphicsProperties.maxSwapchainImageWidth = EncoderLimits::kMaxFrameWidth;
    properties->graphicsProperties.maxSwapchainImageHeight = EncoderLimits::kMaxFrameHeight;
    properties->graphicsProperties.maxLayerCount = 1;

    properties->trackingProperties.orientationTracking = XR_TRUE;
    properties->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(
    XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacityInput,
    uint32_t* countOutput, XrEnvironmentBlendMode* modes)
{
    const std::vector<XrEnvironmentBlendMode> supported{ XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    return FillArray(supported, capacityInput, countOutput, modes);
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurations(XrInstance, XrSystemId,
                                                             uint32_t capacityInput,
                                                             uint32_t* countOutput,
                                                             XrViewConfigurationType* types)
{
    const std::vector<XrViewConfigurationType> supported{
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO
    };
    return FillArray(supported, capacityInput, countOutput, types);
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetViewConfigurationProperties(
    XrInstance, XrSystemId, XrViewConfigurationType type,
    XrViewConfigurationProperties* configurationProperties)
{
    if (configurationProperties == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    configurationProperties->viewConfigurationType = type;
    configurationProperties->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(
    XrInstance instance, XrSystemId, XrViewConfigurationType type, uint32_t capacityInput,
    uint32_t* countOutput, XrViewConfigurationView* views)
{
    Instance* self = AsInstance(instance);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }

    // Recommended and maximum are the same value on purpose. The encoder is configured for
    // exactly one frame size and anything else has to be copied or letterboxed into it, so
    // there is nothing to gain by inviting an application to pick something larger.
    const FrameConfig& config =
        self->session ? self->session->config : kDefaultPerEyeConfig;

    XrViewConfigurationView view{ XR_TYPE_VIEW_CONFIGURATION_VIEW };
    view.recommendedImageRectWidth = config.width;
    view.maxImageRectWidth = config.width;
    view.recommendedImageRectHeight = config.height;
    view.maxImageRectHeight = config.height;
    view.recommendedSwapchainSampleCount = 1;
    view.maxSwapchainSampleCount = 1;

    const std::vector<XrViewConfigurationView> supported(kViewCount, view);
    return FillArray(supported, capacityInput, countOutput, views);
}

XRAPI_ATTR XrResult XRAPI_CALL
xrGetD3D11GraphicsRequirementsKHR(XrInstance, XrSystemId,
                                  XrGraphicsRequirementsD3D11KHR* graphicsRequirements)
{
    if (graphicsRequirements == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // No adapter is imposed: the application already owns the only GPU in the console, and
    // pinning a LUID here would only give it a way to disagree with itself.
    graphicsRequirements->adapterLuid = LUID{};
    graphicsRequirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

// --- Session ----------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance instance,
                                               const XrSessionCreateInfo* createInfo,
                                               XrSession* session)
{
    Instance* self = AsInstance(instance);
    if (self == nullptr || createInfo == nullptr || session == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (self->session)
    {
        return XR_ERROR_LIMIT_REACHED;
    }

    const auto* binding = static_cast<const XrBaseInStructure*>(createInfo->next);
    const XrGraphicsBindingD3D11KHR* d3d11 = nullptr;
    while (binding != nullptr)
    {
        if (binding->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
        {
            d3d11 = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(binding);
            break;
        }
        binding = binding->next;
    }

    if (d3d11 == nullptr || d3d11->device == nullptr)
    {
        LogError(L"OpenXR: xrCreateSession needs an XrGraphicsBindingD3D11KHR");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    auto created = std::make_unique<Session>();
    created->instance = self;
    created->device.copy_from(d3d11->device);
    // The application's own immediate context, so swapchain writes and our reads are
    // ordered without any cross-device or cross-context synchronisation.
    created->device->GetImmediateContext(created->context.put());

    // The encoder runs its own event thread and touches the device from it, so the context
    // is used from two threads whether or not the application expects that. An immediate
    // context is not thread-safe by default and the resulting corruption is an access
    // violation several frames later, nowhere near the cause.
    //
    // The framework's own device turns this on at creation. Here the application created
    // the device and has no reason to have done so, which makes this the runtime's job.
    winrt::com_ptr<ID3D11Multithread> multithread;
    if (SUCCEEDED(created->context->QueryInterface(IID_PPV_ARGS(multithread.put()))))
    {
        multithread->SetMultithreadProtected(TRUE);
    }
    else
    {
        LogWarn(L"OpenXR: ID3D11Multithread unavailable; the encoder thread may race the "
                L"application's rendering");
    }

    *session = reinterpret_cast<XrSession>(created.get());
    self->session = std::move(created);

    // An application drives its state machine entirely off these events, so the whole
    // ramp to READY is queued at once. There is nothing to wait for - there is no
    // compositor to hand us a slot.
    self->QueueSessionState(*session, XR_SESSION_STATE_IDLE);
    self->QueueSessionState(*session, XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession session)
{
    Session* self = AsSession(session);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    SessionShutdown(*self);
    if (self->instance != nullptr)
    {
        self->instance->session.reset();
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession session,
                                              const XrSessionBeginInfo* beginInfo)
{
    Session* self = AsSession(session);
    if (self == nullptr || beginInfo == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (beginInfo->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (self->running)
    {
        return XR_ERROR_SESSION_RUNNING;
    }

    self->running = true;
    self->state = XR_SESSION_STATE_SYNCHRONIZED;
    self->instance->QueueSessionState(session, XR_SESSION_STATE_SYNCHRONIZED);
    self->instance->QueueSessionState(session, XR_SESSION_STATE_VISIBLE);
    self->instance->QueueSessionState(session, XR_SESSION_STATE_FOCUSED);
    self->state = XR_SESSION_STATE_FOCUSED;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession session)
{
    Session* self = AsSession(session);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!self->running)
    {
        return XR_ERROR_SESSION_NOT_STOPPING;
    }
    self->running = false;
    self->state = XR_SESSION_STATE_IDLE;
    SessionShutdown(*self);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrRequestExitSession(XrSession session)
{
    Session* self = AsSession(session);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    self->exiting = true;
    self->instance->QueueSessionState(session, XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}

// --- Spaces -----------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession session,
                                                          uint32_t capacityInput,
                                                          uint32_t* countOutput,
                                                          XrReferenceSpaceType* spaces)
{
    if (AsSession(session) == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    const std::vector<XrReferenceSpaceType> supported{
        XR_REFERENCE_SPACE_TYPE_VIEW,
        XR_REFERENCE_SPACE_TYPE_LOCAL,
        XR_REFERENCE_SPACE_TYPE_STAGE,
    };
    return FillArray(supported, capacityInput, countOutput, spaces);
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session,
                                                      const XrReferenceSpaceCreateInfo* createInfo,
                                                      XrSpace* space)
{
    Session* self = AsSession(session);
    if (self == nullptr || createInfo == nullptr || space == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    auto created = std::make_unique<Space>();
    created->session = self;
    created->referenceType = createInfo->referenceSpaceType;
    created->poseInSpace = createInfo->poseInReferenceSpace;

    *space = reinterpret_cast<XrSpace>(created.get());
    self->spaces.push_back(std::move(created));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetReferenceSpaceBoundsRect(XrSession, XrReferenceSpaceType,
                                                             XrExtent2Df* bounds)
{
    if (bounds != nullptr)
    {
        *bounds = XrExtent2Df{ 0.0f, 0.0f };
    }
    // No play area is measured on this side; the headset knows its own and the host has no
    // way to learn it over the current protocol.
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession session,
                                                   const XrActionSpaceCreateInfo* createInfo,
                                                   XrSpace* space)
{
    Session* self = AsSession(session);
    if (self == nullptr || createInfo == nullptr || space == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    auto created = std::make_unique<Space>();
    created->session = self;
    created->isActionSpace = true;
    created->action = AsAction(createInfo->action);
    created->subactionPath = createInfo->subactionPath;
    created->poseInSpace = createInfo->poseInActionSpace;

    *space = reinterpret_cast<XrSpace>(created.get());
    self->spaces.push_back(std::move(created));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace space)
{
    Space* self = AsSpace(space);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    Session* session = self->session;
    if (session != nullptr)
    {
        auto& list = session->spaces;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [self](const std::unique_ptr<Space>& entry) {
                                      return entry.get() == self;
                                  }),
                   list.end());
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime,
                                             XrSpaceLocation* location)
{
    XVR_TRACE_CALL(L"xrLocateSpace");
    Space* self = AsSpace(space);
    Space* base = AsSpace(baseSpace);
    if (self == nullptr || base == nullptr || location == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    Session* session = self->session;
    FrameContext frame;
    {
        std::lock_guard lock(session->frameMutex);
        frame = session->frame;
    }

    location->locationFlags = 0;
    location->pose = IdentityPose();

    XrPosef inTracking;
    bool valid = false;

    if (self->isActionSpace)
    {
        const Action* action = self->action;
        if (action != nullptr)
        {
            int hand = HandForSubaction(*session->instance, self->subactionPath);
            Action::Binding binding = Action::Binding::GripPose;
            if (hand < 0)
            {
                int resolved = 0;
                if (ResolveAction(*session->instance, *action, XR_NULL_PATH, frame, resolved,
                                  binding))
                {
                    hand = resolved;
                }
            }
            else
            {
                binding = action->bindings[hand];
            }

            const ControllerInput* controller = ControllerFor(frame, hand);
            if (controller != nullptr && controller->active)
            {
                inTracking = binding == Action::Binding::AimPose
                                 ? ToXrPose(controller->aimPosition, controller->aimOrientation)
                                 : ToXrPose(controller->gripPosition, controller->gripOrientation);
                inTracking = Multiply(inTracking, self->poseInSpace);
                valid = true;
            }
        }
    }
    else
    {
        inTracking = SpaceToTracking(*self, frame);
        // Valid regardless, for the same reason as xrLocateViews: an application told the
        // view space is unlocatable stops rendering, and the host runs before the headset
        // has connected. The TRACKED bits below still distinguish measured from assumed.
        valid = true;
    }

    if (!valid)
    {
        return XR_SUCCESS;
    }

    const XrPosef baseInTracking = SpaceToTracking(*base, frame);
    location->pose = Multiply(Invert(baseInTracking), inTracking);
    location->locationFlags =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (frame.poseValid)
    {
        location->locationFlags |= XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                                   XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession session,
                                             const XrViewLocateInfo* viewLocateInfo,
                                             XrViewState* viewState, uint32_t viewCapacityInput,
                                             uint32_t* viewCountOutput, XrView* views)
{
    XVR_TRACE_CALL(L"xrLocateViews");
    Session* self = AsSession(session);
    if (self == nullptr || viewLocateInfo == nullptr || viewState == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (viewLocateInfo->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }

    if (viewCountOutput != nullptr)
    {
        *viewCountOutput = kViewCount;
    }
    if (viewCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (viewCapacityInput < kViewCount || views == nullptr)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    FrameContext frame;
    {
        std::lock_guard lock(self->frameMutex);
        frame = self->frame;
    }

    // Always valid, even before a client has connected.
    //
    // Reporting no valid bits was wrong. An application that is told the runtime does not
    // know where the head is will - correctly - decline to render, and PrimedGun does exactly
    // that: it acquires and releases swapchain images and then submits xrEndFrame with zero
    // layers, so nothing is ever encoded and the headset stays black. Since the host starts
    // before the headset has found it, that state lasts for the entire first stretch of every
    // session.
    //
    // A local runtime always has a pose to give, because the hardware is attached. Ours is at
    // the far end of a network, so the honest equivalent is to hand back the default viewpoint
    // and let the application render normally; the TRACKED bits stay off while there is no
    // client, which is the accurate statement that this pose is not being measured.
    viewState->viewStateFlags =
        XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (frame.poseValid)
    {
        viewState->viewStateFlags |=
            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    }

    Space* base = AsSpace(viewLocateInfo->space);
    const XrPosef baseInTracking = base != nullptr ? SpaceToTracking(*base, frame) : IdentityPose();
    const XrPosef trackingToBase = Invert(baseInTracking);

    for (uint32_t eye = 0; eye < kViewCount; ++eye)
    {
        const EyeViewpoint& source = frame.eyes[eye];
        views[eye].type = XR_TYPE_VIEW;
        views[eye].next = nullptr;
        views[eye].pose =
            Multiply(trackingToBase, ToXrPose(source.position, source.orientation));
        // The headset's own asymmetric frustum, passed through unchanged. Substituting a
        // symmetric one here would misplace everything the application renders, and it
        // would look like a projection bug in the application rather than in the runtime.
        views[eye].fov.angleLeft = source.fov[0];
        views[eye].fov.angleRight = source.fov[1];
        views[eye].fov.angleUp = source.fov[2];
        views[eye].fov.angleDown = source.fov[3];
    }
    return XR_SUCCESS;
}

// --- Swapchains -------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession session,
                                                           uint32_t capacityInput,
                                                           uint32_t* countOutput, int64_t* formats)
{
    if (AsSession(session) == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    // sRGB first: applications generally take the first entry they recognise, and the
    // linear variants are here only for those that insist on managing the conversion
    // themselves.
    const std::vector<int64_t> supported{
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM,      DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_D32_FLOAT,           DXGI_FORMAT_D24_UNORM_S8_UINT,
    };
    return FillArray(supported, capacityInput, countOutput, formats);
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession session,
                                                 const XrSwapchainCreateInfo* createInfo,
                                                 XrSwapchain* swapchain)
{
    Session* self = AsSession(session);
    if (self == nullptr || createInfo == nullptr || swapchain == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    auto created = std::make_unique<Swapchain>();
    created->session = self;
    created->info = *createInfo;

    // XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT means the application intends to create views
    // with a different but compatible format to the one it declared - typically declaring
    // sRGB so the compositor decodes correctly, then making a UNORM view to write already
    // encoded bytes without a second gamma pass. A typed texture cannot do that: D3D11 only
    // allows a view format to differ from the resource format when the resource is TYPELESS.
    //
    // Handing back a typed sRGB texture here left the application unable even to classify
    // the texture it had been given, since a _UNORM_SRGB DXGI format has no counterpart in
    // its own format enum while the _TYPELESS one does.
    auto typelessFor = [](DXGI_FORMAT format) -> DXGI_FORMAT {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24G8_TYPELESS;
        default:
            return format;
        }
    };

    const bool isDepth = createInfo->format == DXGI_FORMAT_D32_FLOAT ||
                         createInfo->format == DXGI_FORMAT_D24_UNORM_S8_UINT;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = createInfo->width;
    desc.Height = createInfo->height;
    desc.MipLevels = std::max<uint32_t>(createInfo->mipCount, 1);
    desc.ArraySize = std::max<uint32_t>(createInfo->arraySize, 1);
    const DXGI_FORMAT requestedFormat = static_cast<DXGI_FORMAT>(createInfo->format);
    desc.Format = (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) != 0
                      ? typelessFor(requestedFormat)
                      : requestedFormat;
    desc.SampleDesc.Count = std::max<uint32_t>(createInfo->sampleCount, 1);
    desc.Usage = D3D11_USAGE_DEFAULT;

    desc.BindFlags = isDepth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
    if ((createInfo->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) != 0 || !isDepth)
    {
        // Colour targets always get SHADER_RESOURCE whether or not the application asked
        // for it, because the encode path reads them back after the frame is released.
        desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if ((createInfo->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0)
    {
        desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }

    // Three images, which is the usual depth: the application renders into one while the
    // encoder is still reading the previous, and the third absorbs a late frame without
    // stalling the acquire.
    constexpr uint32_t kImageCount = 3;
    for (uint32_t i = 0; i < kImageCount; ++i)
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        if (FAILED(self->device->CreateTexture2D(&desc, nullptr, texture.put())))
        {
            LogError(L"OpenXR: swapchain texture creation failed ({}x{})", createInfo->width,
                     createInfo->height);
            return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        }
        created->images.push_back(std::move(texture));
    }

    *swapchain = reinterpret_cast<XrSwapchain>(created.get());
    self->swapchains.push_back(std::move(created));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain)
{
    Swapchain* self = AsSwapchain(swapchain);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    Session* session = self->session;
    if (session != nullptr)
    {
        auto& list = session->swapchains;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [self](const std::unique_ptr<Swapchain>& entry) {
                                      return entry.get() == self;
                                  }),
                   list.end());
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                                          uint32_t capacityInput,
                                                          uint32_t* countOutput,
                                                          XrSwapchainImageBaseHeader* images)
{
    Swapchain* self = AsSwapchain(swapchain);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (countOutput != nullptr)
    {
        *countOutput = static_cast<uint32_t>(self->images.size());
    }
    if (capacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (capacityInput < self->images.size() || images == nullptr)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    auto* typed = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    for (size_t i = 0; i < self->images.size(); ++i)
    {
        typed[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        typed[i].next = nullptr;
        typed[i].texture = self->images[i].get();
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain swapchain,
                                                       const XrSwapchainImageAcquireInfo*,
                                                       uint32_t* index)
{
    XVR_TRACE_CALL(L"xrAcquireSwapchainImage");
    Swapchain* self = AsSwapchain(swapchain);
    if (self == nullptr || index == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (self->imageAcquired)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    self->acquiredIndex = (self->acquiredIndex + 1) % static_cast<uint32_t>(self->images.size());
    self->imageAcquired = true;
    self->imageWaited = false;
    *index = self->acquiredIndex;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain swapchain,
                                                    const XrSwapchainImageWaitInfo*)
{
    XVR_TRACE_CALL(L"xrWaitSwapchainImage");
    Swapchain* self = AsSwapchain(swapchain);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!self->imageAcquired)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    // Nothing to wait on. The image was released to the encoder on the same immediate
    // context the application renders with, so the GPU has already ordered the work.
    self->imageWaited = true;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                       const XrSwapchainImageReleaseInfo*)
{
    XVR_TRACE_CALL(L"xrReleaseSwapchainImage");
    Swapchain* self = AsSwapchain(swapchain);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!self->imageAcquired)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    self->imageAcquired = false;
    return XR_SUCCESS;
}

// --- Frame loop -------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession session, const XrFrameWaitInfo*,
                                           XrFrameState* frameState)
{
    XVR_TRACE_CALL(L"xrWaitFrame");
    Session* self = AsSession(session);
    if (self == nullptr || frameState == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!self->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    frameState->type = XR_TYPE_FRAME_STATE;
    return SessionWaitFrame(*self, *frameState);
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession session, const XrFrameBeginInfo*)
{
    XVR_TRACE_CALL(L"xrBeginFrame");
    Session* self = AsSession(session);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!self->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    const bool discarded = self->frameBegun;
    self->frameBegun = true;
    // Two xrBeginFrame calls without an xrEndFrame between them means the application threw
    // the previous frame away. That is legal, and the spec has a specific success code for
    // it rather than an error.
    return discarded ? XR_FRAME_DISCARDED : XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo)
{
    XVR_TRACE_CALL(L"xrEndFrame");
    Session* self = AsSession(session);
    if (self == nullptr || frameEndInfo == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!self->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (!self->frameBegun)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    self->frameBegun = false;
    return SessionSubmitFrame(*self, *frameEndInfo);
}

// --- Actions ----------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance instance, const char* pathString,
                                              XrPath* path)
{
    Instance* self = AsInstance(instance);
    if (self == nullptr || pathString == nullptr || path == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    *path = self->paths.Intern(pathString);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(XrInstance instance, XrPath path,
                                              uint32_t bufferCapacityInput,
                                              uint32_t* bufferCountOutput, char* buffer)
{
    Instance* self = AsInstance(instance);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::string text;
    if (!self->paths.Lookup(path, text))
    {
        return XR_ERROR_PATH_INVALID;
    }
    return FillString(text.c_str(), bufferCapacityInput, bufferCountOutput, buffer);
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance instance,
                                                 const XrActionSetCreateInfo* createInfo,
                                                 XrActionSet* actionSet)
{
    Instance* self = AsInstance(instance);
    if (self == nullptr || createInfo == nullptr || actionSet == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    auto created = std::make_unique<ActionSet>();
    created->instance = self;
    created->name = createInfo->actionSetName;
    created->localizedName = createInfo->localizedActionSetName;
    created->priority = createInfo->priority;

    *actionSet = reinterpret_cast<XrActionSet>(created.get());
    self->actionSets.push_back(std::move(created));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet actionSet)
{
    return AsActionSet(actionSet) != nullptr ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet actionSet,
                                              const XrActionCreateInfo* createInfo,
                                              XrAction* action)
{
    ActionSet* self = AsActionSet(actionSet);
    if (self == nullptr || createInfo == nullptr || action == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (self->attached)
    {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }

    auto created = std::make_unique<Action>();
    created->actionSet = self;
    created->type = createInfo->actionType;
    created->name = createInfo->actionName;
    created->localizedName = createInfo->localizedActionName;
    for (uint32_t i = 0; i < createInfo->countSubactionPaths; ++i)
    {
        created->subactionPaths.push_back(createInfo->subactionPaths[i]);
    }

    *action = reinterpret_cast<XrAction>(created.get());
    self->actions.push_back(std::move(created));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction action)
{
    return AsAction(action) != nullptr ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings)
{
    Instance* self = AsInstance(instance);
    if (self == nullptr || suggestedBindings == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    // Every profile is accepted and mapped onto the same controller state. The client sends
    // Touch controllers, but an application that only ever suggests bindings for, say, the
    // simple controller profile would otherwise get no input at all - and refusing the
    // profile would not help it, because it has nothing else to offer.
    for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; ++i)
    {
        const XrActionSuggestedBinding& suggestion = suggestedBindings->suggestedBindings[i];
        Action* action = AsAction(suggestion.action);
        if (action == nullptr)
        {
            continue;
        }

        std::string text;
        if (!self->paths.Lookup(suggestion.binding, text))
        {
            continue;
        }

        const int hand = HandFromPath(text);
        const Action::Binding binding = BindingFromPath(text);
        if (binding == Action::Binding::None)
        {
            continue;
        }

        if (hand < 0)
        {
            action->bindings[0] = binding;
            action->bindings[1] = binding;
        }
        else
        {
            action->bindings[hand] = binding;
        }
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession session,
                                                         const XrSessionActionSetsAttachInfo* info)
{
    Session* self = AsSession(session);
    if (self == nullptr || info == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (self->actionsAttached)
    {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }

    for (uint32_t i = 0; i < info->countActionSets; ++i)
    {
        ActionSet* set = AsActionSet(info->actionSets[i]);
        if (set != nullptr)
        {
            set->attached = true;
            self->attachedActionSets.push_back(set);
        }
    }
    self->actionsAttached = true;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(
    XrSession session, XrPath, XrInteractionProfileState* interactionProfile)
{
    Session* self = AsSession(session);
    if (self == nullptr || interactionProfile == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    interactionProfile->interactionProfile =
        self->instance->paths.Intern("/interaction_profiles/oculus/touch_controller");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession session, const XrActionsSyncInfo*)
{
    XVR_TRACE_CALL(L"xrSyncActions");
    Session* self = AsSession(session);
    if (self == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!self->actionsAttached)
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }

    self->actionFramePrev = self->actionFrame;
    {
        std::lock_guard lock(self->frameMutex);
        self->actionFrame = self->frame;
    }
    self->actionSyncTime = NowXrTime();

    // Reported unfocused when no client is attached, which is how an application learns to
    // pause rather than run against controllers that are not there.
    return self->clientConnected ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession session,
                                                       const XrActionStateGetInfo* getInfo,
                                                       XrActionStateBoolean* state)
{
    Session* self = AsSession(session);
    if (self == nullptr || getInfo == nullptr || state == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    Action* action = AsAction(getInfo->action);
    if (action == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    state->isActive = XR_FALSE;
    state->currentState = XR_FALSE;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = self->actionSyncTime;

    int hand = 0;
    Action::Binding binding = Action::Binding::None;
    if (!ResolveAction(*self->instance, *action, getInfo->subactionPath, self->actionFrame, hand,
                       binding))
    {
        return XR_SUCCESS;
    }

    const bool now = ButtonDown(self->actionFrame.controllers[hand], binding);
    const bool before = ButtonDown(self->actionFramePrev.controllers[hand], binding);

    state->isActive = XR_TRUE;
    state->currentState = now ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = now != before ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession session,
                                                     const XrActionStateGetInfo* getInfo,
                                                     XrActionStateFloat* state)
{
    Session* self = AsSession(session);
    if (self == nullptr || getInfo == nullptr || state == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    Action* action = AsAction(getInfo->action);
    if (action == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    state->isActive = XR_FALSE;
    state->currentState = 0.0f;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = self->actionSyncTime;

    int hand = 0;
    Action::Binding binding = Action::Binding::None;
    if (!ResolveAction(*self->instance, *action, getInfo->subactionPath, self->actionFrame, hand,
                       binding))
    {
        return XR_SUCCESS;
    }

    const float now = AxisValue(self->actionFrame.controllers[hand], binding);
    const float before = AxisValue(self->actionFramePrev.controllers[hand], binding);

    state->isActive = XR_TRUE;
    state->currentState = now;
    state->changedSinceLastSync = now != before ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession session,
                                                        const XrActionStateGetInfo* getInfo,
                                                        XrActionStateVector2f* state)
{
    Session* self = AsSession(session);
    if (self == nullptr || getInfo == nullptr || state == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    Action* action = AsAction(getInfo->action);
    if (action == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    state->isActive = XR_FALSE;
    state->currentState = XrVector2f{ 0.0f, 0.0f };
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = self->actionSyncTime;

    int hand = 0;
    Action::Binding binding = Action::Binding::None;
    if (!ResolveAction(*self->instance, *action, getInfo->subactionPath, self->actionFrame, hand,
                       binding))
    {
        return XR_SUCCESS;
    }
    if (binding != Action::Binding::Thumbstick)
    {
        return XR_SUCCESS;
    }

    const ControllerInput& now = self->actionFrame.controllers[hand];
    const ControllerInput& before = self->actionFramePrev.controllers[hand];

    state->isActive = XR_TRUE;
    state->currentState = XrVector2f{ now.thumbstick[0], now.thumbstick[1] };
    state->changedSinceLastSync = (now.thumbstick[0] != before.thumbstick[0] ||
                                   now.thumbstick[1] != before.thumbstick[1])
                                      ? XR_TRUE
                                      : XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession session,
                                                    const XrActionStateGetInfo* getInfo,
                                                    XrActionStatePose* state)
{
    Session* self = AsSession(session);
    if (self == nullptr || getInfo == nullptr || state == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    Action* action = AsAction(getInfo->action);
    if (action == nullptr)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    int hand = 0;
    Action::Binding binding = Action::Binding::None;
    state->isActive = ResolveAction(*self->instance, *action, getInfo->subactionPath,
                                    self->actionFrame, hand, binding)
                          ? XR_TRUE
                          : XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateBoundSourcesForAction(
    XrSession, const XrBoundSourcesForActionEnumerateInfo*, uint32_t, uint32_t* countOutput,
    XrPath*)
{
    if (countOutput != nullptr)
    {
        *countOutput = 0;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetInputSourceLocalizedName(
    XrSession, const XrInputSourceLocalizedNameGetInfo*, uint32_t bufferCapacityInput,
    uint32_t* bufferCountOutput, char* buffer)
{
    return FillString("Streamed controller", bufferCapacityInput, bufferCountOutput, buffer);
}

XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession, const XrHapticActionInfo*,
                                                     const XrHapticBaseHeader*)
{
    // Accepted and dropped. There is no haptic channel in the protocol yet, and returning
    // an error would make applications treat working controllers as broken ones.
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession, const XrHapticActionInfo*)
{
    return XR_SUCCESS;
}

// --- Dispatch ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                     PFN_xrVoidFunction* function)
{
    if (name == nullptr || function == nullptr)
    {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *function = nullptr;

#define XVR_ENTRY(fn)                                                                             \
    if (std::strcmp(name, #fn) == 0)                                                              \
    {                                                                                             \
        *function = reinterpret_cast<PFN_xrVoidFunction>(fn);                                     \
        return XR_SUCCESS;                                                                        \
    }

    XVR_ENTRY(xrGetInstanceProcAddr)
    XVR_ENTRY(xrEnumerateApiLayerProperties)
    XVR_ENTRY(xrEnumerateInstanceExtensionProperties)
    XVR_ENTRY(xrCreateInstance)
    XVR_ENTRY(xrDestroyInstance)
    XVR_ENTRY(xrGetInstanceProperties)
    XVR_ENTRY(xrPollEvent)
    XVR_ENTRY(xrResultToString)
    XVR_ENTRY(xrStructureTypeToString)
    XVR_ENTRY(xrGetSystem)
    XVR_ENTRY(xrGetSystemProperties)
    XVR_ENTRY(xrEnumerateEnvironmentBlendModes)
    XVR_ENTRY(xrEnumerateViewConfigurations)
    XVR_ENTRY(xrGetViewConfigurationProperties)
    XVR_ENTRY(xrEnumerateViewConfigurationViews)
    XVR_ENTRY(xrGetD3D11GraphicsRequirementsKHR)
    XVR_ENTRY(xrCreateSession)
    XVR_ENTRY(xrDestroySession)
    XVR_ENTRY(xrBeginSession)
    XVR_ENTRY(xrEndSession)
    XVR_ENTRY(xrRequestExitSession)
    XVR_ENTRY(xrEnumerateReferenceSpaces)
    XVR_ENTRY(xrCreateReferenceSpace)
    XVR_ENTRY(xrGetReferenceSpaceBoundsRect)
    XVR_ENTRY(xrCreateActionSpace)
    XVR_ENTRY(xrDestroySpace)
    XVR_ENTRY(xrLocateSpace)
    XVR_ENTRY(xrLocateViews)
    XVR_ENTRY(xrEnumerateSwapchainFormats)
    XVR_ENTRY(xrCreateSwapchain)
    XVR_ENTRY(xrDestroySwapchain)
    XVR_ENTRY(xrEnumerateSwapchainImages)
    XVR_ENTRY(xrAcquireSwapchainImage)
    XVR_ENTRY(xrWaitSwapchainImage)
    XVR_ENTRY(xrReleaseSwapchainImage)
    XVR_ENTRY(xrWaitFrame)
    XVR_ENTRY(xrBeginFrame)
    XVR_ENTRY(xrEndFrame)
    XVR_ENTRY(xrStringToPath)
    XVR_ENTRY(xrPathToString)
    XVR_ENTRY(xrCreateActionSet)
    XVR_ENTRY(xrDestroyActionSet)
    XVR_ENTRY(xrCreateAction)
    XVR_ENTRY(xrDestroyAction)
    XVR_ENTRY(xrSuggestInteractionProfileBindings)
    XVR_ENTRY(xrAttachSessionActionSets)
    XVR_ENTRY(xrGetCurrentInteractionProfile)
    XVR_ENTRY(xrSyncActions)
    XVR_ENTRY(xrGetActionStateBoolean)
    XVR_ENTRY(xrGetActionStateFloat)
    XVR_ENTRY(xrGetActionStateVector2f)
    XVR_ENTRY(xrGetActionStatePose)
    XVR_ENTRY(xrEnumerateBoundSourcesForAction)
    XVR_ENTRY(xrGetInputSourceLocalizedName)
    XVR_ENTRY(xrApplyHapticFeedback)
    XVR_ENTRY(xrStopHapticFeedback)

#undef XVR_ENTRY

    (void)instance;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

} // extern "C"
