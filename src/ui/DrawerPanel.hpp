#pragma once

#include "bridge/ScrcpyBridge.hpp"
#include "config/AppSettings.hpp"
#include "config/Profile.hpp"
#include "ui/EditorState.hpp"
#include "ui/GdiPlusDraw.hpp"
#include "video/LatencyTelemetry.hpp"

#include <string>
#include <vector>

enum DrawerActionId {
    DA_TAB_0 = 200,
    DA_TAB_1,
    DA_TAB_2,
    DA_TAB_3,
    DA_TAB_4,
    DA_TAB_5,
    DA_TAB_6,
    DA_CLOSE,
    DA_SAVE,

    // Keys tab
    DA_ADD_NODE,        // extra = KeyActionType
    DA_FOCUS_PALETTE_SEARCH,
    DA_TOGGLE_EDIT_TEST,
    DA_CLEAR_BUTTONS,
    DA_INSP_LABEL,
    DA_INSP_REBIND,
    DA_INSP_MOUSE_VK,   // extra = VK
    DA_INSP_ACT_PREV,
    DA_INSP_ACT_NEXT,
    DA_INSP_SCOPE,
    DA_INSP_TOGGLE_MODE,
    DA_INSP_RSTVIEW,
    DA_INSP_REJOY,
    DA_INSP_QUICK_CAST,
    DA_REBIND_CAST_CANCEL,
    DA_CLEAR_CAST_CANCEL,
    DA_INSP_DELETE,

    // Sens & Physics tab
    DA_FIRE_CLAW,
    DA_FIRE_DRAG,
    DA_FIRE_DUAL,
    DA_BOUNDS_UNBOUNDED,
    DA_BOUNDS_BOXED,
    DA_POLL_125,
    DA_POLL_250,
    DA_POLL_500,
    DA_POLL_MAX,
    DA_JOY_GLIDE,
    DA_JOY_FAST,
    DA_TOGGLE_INVERT_Y,
    DA_TOGGLE_REMEMBER_CURSOR,
    DA_REBIND_TOGGLE_AIM,
    DA_SET_TOGGLE_MMB,
    DA_REBIND_HOLD_UNLOCK,
    DA_CLEAR_HOLD_UNLOCK,
    DA_RESET_SENS,
    DA_TOGGLE_ADVANCED,
    DA_REBIND_RESET_TOUCH,
    DA_REBIND_JOY_UP,
    DA_REBIND_JOY_DOWN,
    DA_REBIND_JOY_LEFT,
    DA_REBIND_JOY_RIGHT,
    DA_REBIND_JOY_SPRINT,
    DA_REBIND_JOY_WALK,
    DA_CLEAR_JOY_WALK,

    // Profiles tab
    DA_FOCUS_NEW_NAME,
    DA_CREATE_BLANK,
    DA_CREATE_CLONE,
    DA_CREATE_TEMPLATE, // extra = ProfileTemplateKind
    DA_IMPORT_PROFILE,
    DA_EXPORT_PROFILE,
    DA_COPY_PROFILE_TEXT,
    DA_LOAD_PROFILE,    // extra = idx
    DA_DELETE_PROFILE,  // extra = idx
    DA_TOGGLE_KEYMAP_MODE,
    DA_LINK_CURRENT_APP,
    DA_UNLINK_APP,
    DA_TOGGLE_EDGE_ANCHOR,
    DA_CALIBRATE_ASPECT,

    // Device & Stream tab
    DA_STREAM_PRESET,   // extra = StreamPresetId
    DA_STREAM_FPS,      // extra = 0/60/90/120
    DA_STREAM_BITRATE,  // extra = 8/12/16/24
    DA_STREAM_MAXSIZE,  // extra = 0/1920/1600/1280
    DA_CODEC_OPTS,      // extra = 0 (default), 1 (CBR+NoB), 2 (WiFi intra)
    DA_CYCLE_ENCODER,
    DA_TOGGLE_NATIVE_VIDEO,
    DA_TOGGLE_VSYNC,
    DA_TOGGLE_VIRTUAL_DISPLAY,
    DA_VD_PRESET,       // extra = 0 (1080p), 1 (720p), 2 (auto)
    DA_TOGGLE_GAMEPAD,
    DA_TOGGLE_AUDIO,
    DA_AUDIO_BUFFER,    // extra = 20 / 25 / 30
    DA_TOGGLE_AUDIO_RAW,
    DA_TOGGLE_SCREEN_OFF,
    DA_TOGGLE_STAY_AWAKE,
    DA_TOGGLE_AUTO_SWITCH,
    DA_TOGGLE_RCLICK_BACK,
    DA_TOGGLE_CLIPBOARD_SYNC,
    DA_SET_LANGUAGE,      // extra = 0..5
    DA_TOGGLE_DEV_ADVANCED,
    DA_APPLY_RECONNECT,
    DA_DISCONNECT,
    DA_RUN_LATENCY_PROBE,
    DA_FOCUS_WIRELESS,
    DA_CONNECT_WIRELESS,
    DA_OPEN_ONBOARDING,
    DA_ADD_PHONE_TAB,

    // Gamepad v2 tab (F6)
    DA_PAD_MODE,          // extra = 0 (TOUCH) / 1 (NATIVE)
    DA_PAD_BIND_BTN,      // extra = PadButtonId (0..15)
    DA_PAD_ASSIGN_TARGET, // extra = -1 (Clear), -2 (FIRE), -3 (SCOPE), >=0 (button idx)
    DA_PAD_RESET_MAP,

    // Apps & Second Phone tab (F8)
    DA_APPS_FOCUS_SEARCH,
    DA_APPS_CLEAR_SEARCH,
    DA_APPS_REFRESH,
    DA_APPS_TOGGLE_SYSTEM,
    DA_APP_TOGGLE_PIN,        // extra = index in installedApps_
    DA_APP_OPEN_SECOND_PHONE, // extra = index in installedApps_
    DA_APP_LAUNCH_CURRENT,    // extra = index in installedApps_
    DA_APP_CREATE_SHORTCUT,   // extra = index in installedApps_
    DA_OPEN_APPS_TAB,

    // Two-Way Files tab (F9)
    DA_FILES_UP,
    DA_FILES_REFRESH,
    DA_FILES_JUMP_DIR,        // extra = 0:/sdcard, 1:/sdcard/Download, 2:/sdcard/DCIM, 3:/sdcard/Pictures
    DA_FILES_PUSH_PICKER,
    DA_FILE_OPEN_DIR,         // extra = index in remoteEntries_
    DA_FILE_PULL,             // extra = index in remoteEntries_
    DA_OPEN_FILES_TAB,

    // F12 Recording & Replay, F13 Streamer & Clean Feed, F14 Device Controls
    DA_TOGGLE_RECORD,
    DA_SAVE_REPLAY,
    DA_TOGGLE_REPLAY_BUF,
    DA_SET_REPLAY_SEC,        // extra = 15 / 30 / 60
    DA_TOGGLE_STREAMER_MODE,
    DA_TOGGLE_CLEAN_FEED,
    DA_TOGGLE_SHOW_TOUCHES,
    DA_TOGGLE_KEYPRESS_OVERLAY,
    DA_DEV_ROTATE,
    DA_DEV_NOTIFS,
    DA_DEV_SETTINGS_PANEL,
    DA_DEV_COLLAPSE_PANELS,
    DA_DEV_WAKE_BACK,
    DA_DEV_POWER,
    DA_DEV_VOL_DN,
    DA_DEV_VOL_UP,
    DA_DEV_VOL_MUTE,
    DA_DEV_SHOT_CLIP
};

struct DrawerActionResult {
    bool profileModified = false;
    bool tuningOnlyModified = false;
    bool settingsModified = false;
    bool requestSave = false;
    bool requestCloseDrawer = false;
    bool requestSwitchProfile = false;
    std::string switchProfilePath;
    bool requestAddNode = false;
    KeyActionType addNodeType = NODE_PRESS;
    std::string addNodeLabel;
    bool requestStartRebindNode = false;
    bool requestDeleteSelectedNode = false;
    bool cancelRebind = false;
    bool requestReconnectStream = false;
    bool requestDisconnectDevice = false;
    bool requestConnectWireless = false;
    std::string wirelessAddr;
    bool requestLinkCurrentApp = false;
    bool requestUnlinkApp = false;
    bool requestCalibrateAspect = false;
    bool requestRunLatencyProbe = false;
    bool vsyncToggled = false;
    bool languageChanged = false;
    bool requestOpenOnboarding = false;
    bool requestAddPhoneTab = false;
    bool requestToggleEditTest = false;
    bool requestCreateTemplate = false;
    bool requestToggleRecord = false;
    bool requestSaveReplay = false;
    bool requestToggleCleanFeed = false;
    bool replayConfigChanged = false;
    bool streamerModeChanged = false;
    int requestDeviceControl = 0; // 1=Rotate..10=ShotClip
    int templateKind = 0;
    bool requestImportProfile = false;
    bool requestExportProfile = false;
    bool requestCopyProfileText = false;
    bool requestClearButtonsUndo = false;
    bool requestDeleteProfileTrash = false;
    std::string deleteProfilePath;
    std::string deleteProfileName;
    bool requestRefreshApps = false;
    bool requestOpenAppSecondPhone = false;
    bool requestLaunchAppCurrent = false;
    bool requestCreateAppShortcut = false;
    std::string appPackage;
    std::string appName;
    bool requestBrowseRemoteDir = false;
    std::string browseRemoteDir;
    bool requestPullRemoteFile = false;
    std::string pullRemotePath;
    std::string pullFileName;
    bool requestPushFilePicker = false;
    std::string pushTargetDir;
    std::string toastMessage;
};

// Docked 356px Studio Drawer placed OUTSIDE the phone viewport (between ViewportHost and SideRail).
// Supports smooth scrolling, hover states, and all 4 tabs:
//   0 = KEYS (10-control palette + live Key Inspector)
//   1 = SENS (Sensitivity sliders, Aim physics, Fire/WASD/Polling, Lock hotkeys)
//   2 = PROFILES (Create Blank/Clone safely, switch, delete)
//   3 = DEVICE (Stream FPS/Bitrate/Resolution/Audio, Wireless ADB, Activity Log)
class DrawerPanel {
public:
    DrawerPanel();

    void setProfilesDir(const std::string& dir, const std::string& activePath);
    void refreshProfileList();

    void setActiveTab(int tab);
    int activeTab() const { return activeTab_; }
    void setLatencyDiagnostics(const TelemetrySummary& vidTel, const ProbeResult& probe, const std::string& presentMode) {
        vidTelemetry_ = vidTel;
        probeResult_ = probe;
        presentModeLabel_ = presentMode;
    }
    void setThermalAndEncoderInfo(const std::string& serial, const PhoneThermalInfo& thermal,
                                  const std::string& activeEncoder, const std::vector<VideoEncoderInfo>& encoders) {
        activeDeviceSerial_ = serial;
        thermalInfo_ = thermal;
        activeEncoder_ = activeEncoder;
        deviceEncoders_ = encoders;
    }
    void setPendingStreamChanges(bool pending) { setPendingStreamChangeCount(pending ? 1 : 0); }
    void setPendingStreamChangeCount(int count) {
        pendingStreamChangeCount_ = (count < 0) ? 0 : count;
        pendingStreamChanges_ = (pendingStreamChangeCount_ > 0);
    }
    void setEditTestLive(bool live) { editTestLive_ = live; }
    void setInstalledApps(const std::vector<DeviceAppInfo>& apps, bool loading, const std::string& statusMsg);
    void setRemoteFiles(const std::string& currentDir, const std::vector<RemoteFileEntry>& entries,
                        bool loading, const std::string& statusMsg);
    const std::string& remoteCurrentDir() const { return remoteCurrentDir_; }
    void setCreatorState(bool recording, int recSec, bool cleanFeedOpen, double replayBufSec) {
        creatorRecording_ = recording;
        creatorRecSec_ = recSec;
        creatorCleanFeedOpen_ = cleanFeedOpen;
        creatorReplayBufSec_ = replayBufSec;
    }

    bool isTypingText() const {
        return editingNewProfileName_ || editingNodeLabel_ || editingWireless_ ||
               editingPaletteSearch_ || editingAppsSearch_;
    }
    bool isWaitingSpecialRebind() const { return specialRebind_ != SRB_NONE; }
    bool isRebindingToggleAim() const { return specialRebind_ == SRB_TOGGLE_AIM; }
    bool isRebindingHoldUnlock() const { return specialRebind_ == SRB_HOLD_UNLOCK; }
    std::string specialRebindLabel() const;
    bool isDraggingSlider() const { return activeSliderId_ != 0; }
    bool isAdvancedExpanded() const { return advancedExpanded_; }
    bool hasKeyboardFocus() const { return focusedIdx_ >= 0; }
    int focusedIndex() const { return focusedIdx_; }
    bool hasSelectedPadButton() const { return activeTab_ == 4 && selectedPadBtn_ >= 0 && selectedPadBtn_ < PAD_BUTTON_COUNT; }
    bool bindSelectedPadButtonToNode(Profile& profile, const std::string& label, std::string& outToast);

    void clearFocus() {
        editingNewProfileName_ = false;
        editingNodeLabel_ = false;
        editingWireless_ = false;
        editingPaletteSearch_ = false;
        editingAppsSearch_ = false;
        focusedIdx_ = -1;
        specialRebind_ = SRB_NONE;
    }

    void paint(Gdiplus::Graphics& g, const RECT& bounds,
               const Profile& profile, const AppSettings& settings,
               const std::string& activeProfilePath,
               int selectedBtnIndex, bool waitingNodeRebind,
               bool connected, const std::string& deviceLabel,
               const std::string& foregroundPkg,
               const std::vector<std::string>& logs,
               int hoverX, int hoverY,
               int slideOffsetX = 0);

    bool containsPoint(int x, int y) const;
    bool hoverItemAt(int x, int y, int& outId, RECT& outRc) const;
    bool rectForAction(int actionId, int extra, RECT& outRc) const;

    bool onMouseDown(int x, int y, Profile& profile, AppSettings& settings,
                     int& selectedBtnIndex, DrawerActionResult& out);
    bool onMouseMove(int x, int y, Profile& profile, int selectedBtnIndex, DrawerActionResult& out);
    bool onMouseUp();
    bool onMouseWheel(int x, int y, int delta);
    bool onKeyDown(int vk, Profile& profile, AppSettings& settings,
                   int selectedBtnIndex, DrawerActionResult& out);
    bool onMouseBind(int mouseVk, Profile& profile, DrawerActionResult& out);
    bool onChar(char ch, Profile& profile, AppSettings& settings,
                int selectedBtnIndex, DrawerActionResult& out);

private:
    struct Hotspot {
        RECT rc;
        int id;
        int extra;
    };

    struct SliderHit {
        RECT trackRc;
        RECT hitRc;
        int id;
        float minVal;
        float maxVal;
    };

    struct FocusableItem {
        RECT rc;
        bool isSlider;
        bool inBody;
        int id;
        int extra;
        float minVal;
        float maxVal;

        FocusableItem()
            : isSlider(false), inBody(false), id(0), extra(0), minVal(0.0f), maxVal(1.0f) {
            ZeroMemory(&rc, sizeof(rc));
        }
    };

    bool activateHotspot(const Hotspot& hs, Profile& profile, AppSettings& settings,
                         int& selectedBtnIndex, DrawerActionResult& out);
    float currentSliderValue(int sliderId, const Profile& p, int selectedBtnIndex) const;

    struct ProfileEntry {
        std::string path;
        std::string fileName;
        std::string displayName;
    };

    void drawSliderRow(Gdiplus::Graphics& g, int x, int y, int w,
                       const char* label, float val, float minV, float maxV,
                       const char* valText, uint32_t accent, int sliderId,
                       int hoverX, int hoverY);
    void drawToggleRow(Gdiplus::Graphics& g, int x, int y, int w,
                       const char* label, bool on, uint32_t accent, int actionId,
                       int hoverX, int hoverY);
    void applySliderValue(int sliderId, float normT, Profile& p, int selectedBtnIndex);

    int paintTabKeys(Gdiplus::Graphics& g, int x, int y, int w,
                     const Profile& p, int selectedBtnIndex, bool waitingRebind,
                     int hoverX, int hoverY);
    int paintTabSens(Gdiplus::Graphics& g, int x, int y, int w,
                     const Profile& p, int hoverX, int hoverY);
    int paintTabProfiles(Gdiplus::Graphics& g, int x, int y, int w,
                         const Profile& p, const std::string& activePath,
                         const std::string& foregroundPkg, int hoverX, int hoverY);
    int paintTabDevice(Gdiplus::Graphics& g, int x, int y, int w,
                       const AppSettings& s, bool connected, const std::string& deviceLabel,
                       const std::vector<std::string>& logs, int hoverX, int hoverY);
    int paintTabGamepad(Gdiplus::Graphics& g, int x, int y, int w,
                        const Profile& p, const AppSettings& s, int hoverX, int hoverY);
    int paintTabApps(Gdiplus::Graphics& g, int x, int y, int w,
                     const AppSettings& s, bool connected, int hoverX, int hoverY);
    int paintTabFiles(Gdiplus::Graphics& g, int x, int y, int w,
                      bool connected, int hoverX, int hoverY);

    RECT bounds_;
    RECT bodyClip_;
    int activeTab_ = 0;
    int scrollY_[7];
    int maxScrollY_ = 0;

    std::vector<Hotspot> hotspots_;
    std::vector<SliderHit> sliders_;
    std::vector<FocusableItem> focusables_;
    int focusedIdx_ = -1;

    int activeSliderId_ = 0;
    RECT activeSliderTrack_;

    std::string profilesDir_;
    std::string activeProfilePath_;
    std::vector<ProfileEntry> profileEntries_;
    std::string newProfileName_ = "my_custom";

    enum SpecialRebindTarget {
        SRB_NONE = 0,
        SRB_TOGGLE_AIM,
        SRB_HOLD_UNLOCK,
        SRB_RESET_TOUCH,
        SRB_JOY_UP,
        SRB_JOY_DOWN,
        SRB_JOY_LEFT,
        SRB_JOY_RIGHT,
        SRB_JOY_SPRINT,
        SRB_JOY_WALK,
        SRB_CAST_CANCEL
    };

    bool editingNewProfileName_ = false;
    bool editingNodeLabel_ = false;
    bool editingWireless_ = false;
    bool editingPaletteSearch_ = false;
    bool editingAppsSearch_ = false;
    bool editTestLive_ = false;
    std::string paletteSearch_;
    std::string appsSearch_;
    bool showSystemApps_ = false;
    bool appsLoading_ = false;
    std::string appsStatusMsg_;
    std::vector<DeviceAppInfo> installedApps_;

    std::string remoteCurrentDir_ = "/sdcard";
    bool filesLoading_ = false;
    std::string filesStatusMsg_;
    std::vector<RemoteFileEntry> remoteEntries_;
    bool advancedExpanded_ = false;
    bool devAdvancedExpanded_ = false;
    bool pendingStreamChanges_ = false;
    int pendingStreamChangeCount_ = 0;
    int selectedPadBtn_ = -1;
    SpecialRebindTarget specialRebind_ = SRB_NONE;

    TelemetrySummary vidTelemetry_;
    ProbeResult probeResult_;
    std::string presentModeLabel_;
    std::string activeDeviceSerial_;
    PhoneThermalInfo thermalInfo_;
    std::string activeEncoder_;
    std::vector<VideoEncoderInfo> deviceEncoders_;

    bool creatorRecording_ = false;
    int creatorRecSec_ = 0;
    bool creatorCleanFeedOpen_ = false;
    double creatorReplayBufSec_ = 0.0;
};
