// Syncing to the Switch's SD card, mounted on this PC (a card reader, or
// hekate's USB mass storage). The card's system.db is both what the Switch
// reads and the record of what's on the card (its `synced` table), so nothing
// else needs to remember the card.
//
// Order of work, so a card pulled out midway is never left pointing at missing files:
//   1. new and changed files are copied to .incoming, then moved into .media
//   2. system.db is updated in one transaction (pictures included)
//   3. files the library no longer has are deleted
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "model.h"
#include "sql.h"

struct Card {
    std::string root;   // "E:/switch/mediacenter"
    std::string label;  // "E: (SWITCH SD)"
    uint64_t freeBytes = 0, totalBytes = 0;
    bool hasApp = false;  // MediaCenter.nro is there
};

// Every drive with a switch/mediacenter folder, the ones with the app first.
std::vector<Card> findCards();

struct SyncProgress {
    std::string action, item;
    uint64_t bytesDone = 0, bytes = 0;
    int stepsDone = 0, steps = 0;
};

struct SyncResult {
    bool ok = false;
    bool nothingChanged = false;
    std::string error;
    int copied = 0, deleted = 0, failed = 0;
    uint64_t bytes = 0;
};

SyncResult syncToCard(Sql& work, const Model& model, const std::string& cardRoot, const std::string& pcName,
                      const std::atomic<bool>& cancel, const std::function<void(const SyncProgress&)>& progress);
