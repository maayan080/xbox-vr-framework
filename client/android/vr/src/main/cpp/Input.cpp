#include "XvrVr.h"

#include <cstring>

// Bit values must match xvr::net::ControllerButtons in the framework's Protocol.h.
namespace {

constexpr uint32_t kButtonPrimaryClick = 1u << 0;
constexpr uint32_t kButtonSecondaryClick = 1u << 2;
constexpr uint32_t kButtonMenuClick = 1u << 4;
constexpr uint32_t kButtonThumbstickClick = 1u << 5;
constexpr uint32_t kButtonActive = 1u << 31;

bool CheckXr(XrResult result, const char* what)
{
    if (XR_SUCCEEDED(result))
    {
        return true;
    }
    XVR_ERR("%s failed: %d", what, static_cast<int>(result));
    return false;
}

} // namespace

namespace xvr {

bool XrApp::CreateInputActions()
{
    XrActionSetCreateInfo actionSetInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
    std::strcpy(actionSetInfo.actionSetName, "gameplay");
    std::strcpy(actionSetInfo.localizedActionSetName, "Gameplay");
    actionSetInfo.priority = 0;

    if (!CheckXr(xrCreateActionSet(m_instance, &actionSetInfo, &m_input.actionSet),
                 "xrCreateActionSet"))
    {
        return false;
    }

    xrStringToPath(m_instance, "/user/hand/left", &m_input.handPaths[0]);
    xrStringToPath(m_instance, "/user/hand/right", &m_input.handPaths[1]);

    // Every action is created for both hands via subaction paths, so the same action can
    // be queried per hand rather than duplicating the whole set.
    const auto createAction = [&](XrAction& action, XrActionType type, const char* name,
                                  const char* localized) {
        XrActionCreateInfo info{ XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = type;
        info.countSubactionPaths = 2;
        info.subactionPaths = m_input.handPaths;
        std::strcpy(info.actionName, name);
        std::strcpy(info.localizedActionName, localized);
        return CheckXr(xrCreateAction(m_input.actionSet, &info, &action), name);
    };

    bool ok = true;
    ok &= createAction(m_input.gripPose, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip Pose");
    ok &= createAction(m_input.aimPose, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose");
    ok &= createAction(m_input.trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger");
    ok &= createAction(m_input.squeeze, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Squeeze");
    ok &= createAction(m_input.thumbstick, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick",
                       "Thumbstick");
    ok &= createAction(m_input.primaryClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "primary",
                       "Primary Button");
    ok &= createAction(m_input.secondaryClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary",
                       "Secondary Button");
    ok &= createAction(m_input.menuClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu");
    ok &= createAction(m_input.thumbstickClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click",
                       "Thumbstick Click");

    if (!ok)
    {
        return false;
    }

    const auto path = [&](const char* text) {
        XrPath result = XR_NULL_PATH;
        xrStringToPath(m_instance, text, &result);
        return result;
    };

    // Bindings are suggested, not assigned: the runtime decides how these actions map onto
    // whatever hardware is actually connected. Naming buttons "primary"/"secondary" rather
    // than A/B/X/Y keeps the same suggestion valid for both hands, where the physical
    // labels differ.
    const XrActionSuggestedBinding bindings[] = {
        { m_input.gripPose, path("/user/hand/left/input/grip/pose") },
        { m_input.gripPose, path("/user/hand/right/input/grip/pose") },
        { m_input.aimPose, path("/user/hand/left/input/aim/pose") },
        { m_input.aimPose, path("/user/hand/right/input/aim/pose") },
        { m_input.trigger, path("/user/hand/left/input/trigger/value") },
        { m_input.trigger, path("/user/hand/right/input/trigger/value") },
        { m_input.squeeze, path("/user/hand/left/input/squeeze/value") },
        { m_input.squeeze, path("/user/hand/right/input/squeeze/value") },
        { m_input.thumbstick, path("/user/hand/left/input/thumbstick") },
        { m_input.thumbstick, path("/user/hand/right/input/thumbstick") },
        { m_input.primaryClick, path("/user/hand/left/input/x/click") },
        { m_input.primaryClick, path("/user/hand/right/input/a/click") },
        { m_input.secondaryClick, path("/user/hand/left/input/y/click") },
        { m_input.secondaryClick, path("/user/hand/right/input/b/click") },
        { m_input.menuClick, path("/user/hand/left/input/menu/click") },
        { m_input.thumbstickClick, path("/user/hand/left/input/thumbstick/click") },
        { m_input.thumbstickClick, path("/user/hand/right/input/thumbstick/click") },
    };

    XrInteractionProfileSuggestedBinding suggested{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
    };
    suggested.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    suggested.suggestedBindings = bindings;
    suggested.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);

    if (!CheckXr(xrSuggestInteractionProfileBindings(m_instance, &suggested),
                 "xrSuggestInteractionProfileBindings"))
    {
        // Not fatal: the session still runs, just without controller input. Losing hands
        // is much better than losing the whole stream.
        XVR_WARN("controller bindings rejected; continuing without input");
    }

    XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &m_input.actionSet;
    if (!CheckXr(xrAttachSessionActionSets(m_session, &attachInfo), "xrAttachSessionActionSets"))
    {
        return false;
    }

    for (int hand = 0; hand < 2; ++hand)
    {
        XrActionSpaceCreateInfo spaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = m_input.gripPose;
        spaceInfo.subactionPath = m_input.handPaths[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        CheckXr(xrCreateActionSpace(m_session, &spaceInfo, &m_input.gripSpaces[hand]),
                "grip action space");

        spaceInfo.action = m_input.aimPose;
        CheckXr(xrCreateActionSpace(m_session, &spaceInfo, &m_input.aimSpaces[hand]),
                "aim action space");
    }

    XVR_LOG("input actions ready");
    return true;
}

void XrApp::UpdateControllers(XrTime predictedDisplayTime)
{
    if (m_input.actionSet == XR_NULL_HANDLE)
    {
        return;
    }

    XrActiveActionSet activeSet{ m_input.actionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;

    if (XR_FAILED(xrSyncActions(m_session, &syncInfo)))
    {
        return;
    }

    for (int hand = 0; hand < 2; ++hand)
    {
        // Layout matches xvr::net::ControllerState field for field, so the Java side can
        // copy it onto the wire without rearranging:
        //   [0] buttons, [1..3] grip position, [4..7] grip orientation,
        //   [8..10] aim position, [11..14] aim orientation,
        //   [15] trigger, [16] squeeze, [17..18] thumbstick
        float* out = m_controllerState + hand * kControllerFloats;
        std::memset(out, 0, sizeof(float) * kControllerFloats);

        // Identity orientations, so an inactive controller does not read as a wild pose.
        out[7] = 1.0f;  // grip orientation w
        out[14] = 1.0f; // aim orientation w

        uint32_t buttons = 0;
        bool anyActive = false;

        XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.subactionPath = m_input.handPaths[hand];

        const auto locatePose = [&](XrSpace space, float* position, float* orientation) {
            if (space == XR_NULL_HANDLE)
            {
                return false;
            }
            XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
            if (XR_FAILED(xrLocateSpace(space, m_space, predictedDisplayTime, &location)))
            {
                return false;
            }

            // Both bits matter: a runtime can report a stale pose as "valid" while no
            // longer tracking, and using that would leave a hand frozen in mid-air.
            constexpr XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                                      XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                                      XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
                                                      XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
            if ((location.locationFlags & required) != required)
            {
                return false;
            }

            position[0] = location.pose.position.x;
            position[1] = location.pose.position.y;
            position[2] = location.pose.position.z;
            orientation[0] = location.pose.orientation.x;
            orientation[1] = location.pose.orientation.y;
            orientation[2] = location.pose.orientation.z;
            orientation[3] = location.pose.orientation.w;
            return true;
        };

        if (locatePose(m_input.gripSpaces[hand], out + 1, out + 4))
        {
            anyActive = true;
        }
        locatePose(m_input.aimSpaces[hand], out + 8, out + 11);

        getInfo.action = m_input.trigger;
        XrActionStateFloat floatState{ XR_TYPE_ACTION_STATE_FLOAT };
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo, &floatState)) &&
            floatState.isActive)
        {
            out[15] = floatState.currentState;
        }

        getInfo.action = m_input.squeeze;
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo, &floatState)) &&
            floatState.isActive)
        {
            out[16] = floatState.currentState;
        }

        getInfo.action = m_input.thumbstick;
        XrActionStateVector2f stickState{ XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &getInfo, &stickState)) &&
            stickState.isActive)
        {
            out[17] = stickState.currentState.x;
            out[18] = stickState.currentState.y;
        }

        const auto readBoolean = [&](XrAction action, uint32_t bit) {
            getInfo.action = action;
            XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &state)) &&
                state.isActive && state.currentState)
            {
                buttons |= bit;
            }
        };

        readBoolean(m_input.primaryClick, kButtonPrimaryClick);
        readBoolean(m_input.secondaryClick, kButtonSecondaryClick);
        readBoolean(m_input.menuClick, kButtonMenuClick);
        readBoolean(m_input.thumbstickClick, kButtonThumbstickClick);

        if (anyActive)
        {
            buttons |= kButtonActive;
        }

        // Bits travel through a float slot; the value is copied bit-for-bit rather than
        // converted, since a 32-bit mask does not survive a float round trip.
        std::memcpy(&out[0], &buttons, sizeof(uint32_t));
    }

    if (m_controllerCallback != nullptr)
    {
        m_controllerCallback(m_controllerState, m_controllerCallbackContext);
    }
}

} // namespace xvr
