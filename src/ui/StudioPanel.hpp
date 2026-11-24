#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>

#include "config/Profile.hpp"
#include "ui/GdiDraw.hpp"

#include <string>
#include <vector>

struct StudioUIResult {
    bool profileModified = false;  // Settings or buttons changed -> push to AimEngine
    bool requestSave = false;      // Save current profile to disk
    bool requestCloseEdit = false; // Exit F2 Edit Mode
    bool requestSwitchProfile = false;
    std::string switchProfilePath;
    bool requestAddNode = false;
    KeyActionType addNodeType = NODE_PRESS;
    std::string addNodeLabel;
    bool requestStartRebindNode = false;
    bool requestDeleteSelectedNode = false;
    bool cancelRebind = false;
    std::string toastMessage;
};

class StudioPanel {
public:
    StudioPanel();

    void setProfilesDir(const std::string& dir, const std::string& activePath);
    void refreshProfileList();

    bool isCollapsed() const { return collapsed_; }
    bool isTypingText() const { return editingNewProfileName_ || editingNodeLabel_; }
    bool isWaitingSpecialRebind() const { return rebindingToggleAim_ || rebindingHoldUnlock_; }
    bool isRebindingToggleAim() const { return rebindingToggleAim_; }
    bool isRebindingHoldUnlock() const { return rebindingHoldUnlock_; }
    bool isDragging() const { return draggingPanel_ || activeSliderId_ != 0; }

    // Draws the movable 4-tab Config Panel + Selected Key Inspector card (if selectedBtnIndex >= 0)
    void paint(HDC hdc, int canvasW, int canvasH,
               const Profile& profile, const std::string& activeProfilePath,
               int selectedBtnIndex, bool waitingForNodeRebind,
               HFONT fontSmall, HFONT fontBold, HFONT fontTitle);

    // Hit-tests whether a point falls inside the Config Panel or Inspector Card
    bool containsPoint(int x, int y) const;

    // Mouse / Keyboard handlers (returns true if event was consumed by panel/inspector)
    bool onMouseDown(int x, int y, int canvasW, int canvasH,
                     Profile& profile, int& selectedBtnIndex, StudioUIResult& out);
    bool onMouseMove(int x, int y, int canvasW, int canvasH,
                     Profile& profile, int selectedBtnIndex, StudioUIResult& out);
    // Returns true if a panel or slider drag just finished (caller should persist profile)
    bool onMouseUp();
    bool onKeyDown(int vk, Profile& profile, int selectedBtnIndex, StudioUIResult& out);
    bool onMouseBind(int mouseVk, Profile& profile, int selectedBtnIndex, StudioUIResult& out);
    bool onChar(char ch, Profile& profile, int selectedBtnIndex, StudioUIResult& out);

    void clearTextFocus() {
        editingNewProfileName_ = false;
        editingNodeLabel_ = false;
        rebindingToggleAim_ = false;
        rebindingHoldUnlock_ = false;
    }

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

    void drawSlider(HDC hdc, int x, int y, int w,
                    const char* label, float val, float minV, float maxV,
                    const char* valText, COLORREF accent, int sliderId,
                    HFONT fontSmall, HFONT fontBold, int labelW = 116);

    void applySliderValue(int sliderId, float normT, Profile& profile, int selectedBtnIndex);

    void paintTabSensitivity(HDC hdc, int x, int y, int w, const Profile& p, HFONT fS, HFONT fB);
    void paintTabAddKeys(HDC hdc, int x, int y, int w, HFONT fS, HFONT fB);
    void paintTabProfiles(HDC hdc, int x, int y, int w, const std::string& activePath, HFONT fS, HFONT fB);
    void paintTabAdvanced(HDC hdc, int x, int y, int w, const Profile& p, HFONT fS, HFONT fB);
    void paintNodeInspector(HDC hdc, int x, int y, int w, const KeyNode& node,
                            bool waitingRebind, HFONT fS, HFONT fB);

    int activeTab_ = 0; // 0=Sensitivity, 1=Add Keys, 2=Profiles, 3=Advanced
    bool collapsed_ = false;

    RECT panelRect_;
    RECT titleBarRect_;
    RECT inspectorRect_;
    bool inspectorVisible_ = false;

    std::vector<Hotspot> hotspots_;
    std::vector<SliderHit> sliders_;

    // Panel dragging state
    bool draggingPanel_ = false;
    int dragOffX_ = 0;
    int dragOffY_ = 0;

    // Active slider dragging state
    int activeSliderId_ = 0;
    RECT activeSliderTrack_;

    // Profiles manager state
    std::string profilesDir_;
    std::string activeProfilePath_;
    struct ProfileEntry {
        std::string path;
        std::string displayName;
        std::string fileName;
    };
    std::vector<ProfileEntry> profileEntries_;
    std::string newProfileName_ = "my_custom";
    bool editingNewProfileName_ = false;

    // Node inspector text editing & global rebind flags
    bool editingNodeLabel_ = false;
    bool rebindingToggleAim_ = false;
    bool rebindingHoldUnlock_ = false;
};
