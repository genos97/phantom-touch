#include "ui/EditorState.hpp"
#include "config/KeyCodes.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cmath>
#include <map>

void ProfileUndoStack::push(const Profile& beforeState, const std::string& desc) {
    Entry e;
    e.snapshot = beforeState;
    e.desc = desc;
    undoStack_.push_back(e);
    if (undoStack_.size() > kMaxDepth) {
        undoStack_.erase(undoStack_.begin());
    }
    redoStack_.clear();
}

bool ProfileUndoStack::undo(Profile& ioCurrent, std::string* outDesc) {
    if (undoStack_.empty()) return false;
    Entry top = undoStack_.back();
    undoStack_.pop_back();

    Entry r;
    r.snapshot = ioCurrent;
    r.desc = top.desc;
    redoStack_.push_back(r);

    ioCurrent = top.snapshot;
    if (outDesc) *outDesc = top.desc;
    return true;
}

bool ProfileUndoStack::redo(Profile& ioCurrent, std::string* outDesc) {
    if (redoStack_.empty()) return false;
    Entry top = redoStack_.back();
    redoStack_.pop_back();

    Entry u;
    u.snapshot = ioCurrent;
    u.desc = top.desc;
    undoStack_.push_back(u);

    ioCurrent = top.snapshot;
    if (outDesc) *outDesc = top.desc;
    return true;
}

void ProfileUndoStack::clear() {
    undoStack_.clear();
    redoStack_.clear();
}

void EditorGeometry::snapAndCollectGuides(const Profile& p, int ignoreBtnIdx,
                                          float& ioX, float& ioY,
                                          bool shiftGridSnap,
                                          std::vector<AlignGuide>& outGuides) {
    outGuides.clear();
    if (shiftGridSnap) {
        ioX = std::floor(ioX * 100.0f + 0.5f) / 100.0f;
        ioY = std::floor(ioY * 100.0f + 0.5f) / 100.0f;
        if (ioX < 0.01f) ioX = 0.01f;
        if (ioX > 0.99f) ioX = 0.99f;
        if (ioY < 0.01f) ioY = 0.01f;
        if (ioY > 0.99f) ioY = 0.99f;
        AlignGuide gx;
        gx.vertical = true;
        gx.pos = ioX;
        gx.spanStart = 0.0f;
        gx.spanEnd = 1.0f;
        AlignGuide gy;
        gy.vertical = false;
        gy.pos = ioY;
        gy.spanStart = 0.0f;
        gy.spanEnd = 1.0f;
        outGuides.push_back(gx);
        outGuides.push_back(gy);
        return;
    }

    struct Anchor { float x; float y; };
    std::vector<Anchor> anchors;
    anchors.push_back(Anchor{ 0.5f, 0.5f });
    anchors.push_back(Anchor{ p.joyX, p.joyY });
    anchors.push_back(Anchor{ p.aimX, p.aimY });
    anchors.push_back(Anchor{ p.fireX, p.fireY });
    anchors.push_back(Anchor{ p.leftFireX, p.leftFireY });
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        if (static_cast<int>(i) == ignoreBtnIdx) continue;
        anchors.push_back(Anchor{ p.buttons[i].x, p.buttons[i].y });
    }

    const float kSnapTol = 0.008f;
    float bestDx = kSnapTol;
    float bestX = ioX;
    float bestXSpan0 = ioY, bestXSpan1 = ioY;
    bool snappedX = false;

    float bestDy = kSnapTol;
    float bestY = ioY;
    float bestYSpan0 = ioX, bestYSpan1 = ioX;
    bool snappedY = false;

    for (size_t i = 0; i < anchors.size(); ++i) {
        float dx = std::fabs(ioX - anchors[i].x);
        if (dx < bestDx) {
            bestDx = dx;
            bestX = anchors[i].x;
            bestXSpan0 = (ioY < anchors[i].y) ? ioY : anchors[i].y;
            bestXSpan1 = (ioY > anchors[i].y) ? ioY : anchors[i].y;
            snappedX = true;
        }
        float dy = std::fabs(ioY - anchors[i].y);
        if (dy < bestDy) {
            bestDy = dy;
            bestY = anchors[i].y;
            bestYSpan0 = (ioX < anchors[i].x) ? ioX : anchors[i].x;
            bestYSpan1 = (ioX > anchors[i].x) ? ioX : anchors[i].x;
            snappedY = true;
        }
    }

    if (snappedX) {
        ioX = bestX;
        AlignGuide g;
        g.vertical = true;
        g.pos = ioX;
        g.spanStart = bestXSpan0;
        g.spanEnd = bestXSpan1;
        outGuides.push_back(g);
    }
    if (snappedY) {
        ioY = bestY;
        AlignGuide g;
        g.vertical = false;
        g.pos = ioY;
        g.spanStart = bestYSpan0;
        g.spanEnd = bestYSpan1;
        outGuides.push_back(g);
    }
}

std::vector<KeyConflict> EditorGeometry::detectConflicts(const Profile& p, bool isAimMode) {
    std::vector<KeyConflict> out;
    std::map<uint32_t, int> keyCounts;
    auto bindKey = [](int comboVk, int vk) -> uint32_t {
        return (static_cast<uint32_t>(comboVk & 0xFFFF) << 16) | static_cast<uint32_t>(vk & 0xFFFF);
    };
    auto isMouseVk = [](int vk) -> bool {
        return (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
                vk == VK_XBUTTON1 || vk == VK_XBUTTON2);
    };

    bool hasSkillCast = false;
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        uint32_t k = bindKey(p.buttons[i].comboVk, p.buttons[i].vk);
        if (p.buttons[i].vk != 0) keyCounts[k]++;
        if (p.buttons[i].action == NODE_SKILL_CAST) hasSkillCast = true;
    }

    for (size_t i = 0; i < p.buttons.size(); ++i) {
        const KeyNode& b = p.buttons[i];
        uint32_t k = bindKey(b.comboVk, b.vk);
        bool dup = (b.vk != 0 && keyCounts[k] > 1);
        bool fireClashes = (b.comboVk == 0 && b.vk != 0 && b.vk == p.fireVk);
        bool cancelClashes = (hasSkillCast && p.castCancelVk != 0 && b.comboVk == 0 && b.vk != 0 &&
                              KeyCodes::matches(b.vk, p.castCancelVk));
        bool mouseBlocked = (!isAimMode && b.comboVk == 0 && isMouseVk(b.vk));

        if (!dup && !fireClashes && !cancelClashes && !mouseBlocked) continue;

        KeyConflict c;
        c.buttonIndex = static_cast<int>(i);
        c.cannotFireInCursor = mouseBlocked;
        if (fireClashes) {
            c.reason = KeyCodes::comboToName(b.comboVk, b.vk) + " conflicts with Fire key";
        } else if (cancelClashes) {
            c.reason = KeyCodes::comboToName(b.comboVk, b.vk) + " conflicts with Skill Cast Cancel key";
        } else if (dup) {
            c.reason = KeyCodes::comboToName(b.comboVk, b.vk) + " is bound multiple times";
        } else {
            c.reason = KeyCodes::toName(b.vk) + " node won't fire in Cursor mode";
        }
        out.push_back(c);
    }
    return out;
}
