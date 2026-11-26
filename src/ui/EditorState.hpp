#pragma once

#include "config/Profile.hpp"

#include <string>
#include <vector>

// X4 Visual Editor alignment guide (normalized 0..1 phone coordinates)
struct AlignGuide {
    bool vertical;
    float pos;
    float spanStart;
    float spanEnd;

    AlignGuide() : vertical(false), pos(0.0f), spanStart(0.0f), spanEnd(1.0f) {}
};

// X4 / N9 Keymap conflict warning
struct KeyConflict {
    int buttonIndex;
    std::string reason;
    bool cannotFireInCursor;

    KeyConflict() : buttonIndex(-1), cannotFireInCursor(false) {}
};

// X4 Bounded Undo / Redo command stack for Profile edits
class ProfileUndoStack {
public:
    static const size_t kMaxDepth = 50;

    void push(const Profile& beforeState, const std::string& desc);
    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }
    bool undo(Profile& ioCurrent, std::string* outDesc = nullptr);
    bool redo(Profile& ioCurrent, std::string* outDesc = nullptr);
    void clear();

private:
    struct Entry {
        Profile snapshot;
        std::string desc;
    };
    std::vector<Entry> undoStack_;
    std::vector<Entry> redoStack_;
};

// X4 Snapping, Alignment Guides & Conflict Detection
class EditorGeometry {
public:
    static void snapAndCollectGuides(const Profile& p, int ignoreBtnIdx,
                                     float& ioX, float& ioY,
                                     bool shiftGridSnap,
                                     std::vector<AlignGuide>& outGuides);

    static std::vector<KeyConflict> detectConflicts(const Profile& p, bool isAimMode);
};
