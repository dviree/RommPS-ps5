// RommPS: lists about the sync, opened from Home and from a game's sheet.
//
//   - Conflicts: saves changed here and on RomM since the last sync. Cross on
//     one asks which copy to keep; the other is backed up.
//   - Activity: the sync log (the payload's /api/events), newest first.
//   - Tracked saves: the saves and states kept in sync, by platform, each
//     with its status (/api/tracked).
//   - A game's saves and states: what's on this console, the versions RomM
//     keeps (Cross twice restores one) and the game's log.
//
// Each is a list of rows (rows.hpp) filled from the payload when it opens;
// Circle closes it.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rommps_app.hpp"
#include "rows.hpp"
#include "ui/glyphs.hpp"

#include <string>
#include <vector>

namespace rommps
{

class SyncView
{
  public:
    enum Kind
    {
        kConflicts,
        kActivity,
        kTracked,
        kGame
    };

    void open(App &app, Kind kind, int rom_id = 0, const std::string &name = "");
    void close() { active_ = false; }
    bool active() const { return active_; }
    Kind kind() const { return kind_; }

    void update(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app, float clock) const;
    std::vector<hui::ui::Hint> hints() const;

  private:
    // What Cross does on a row.
    enum Do
    {
        kNothing,
        kPickConflict, // opens the choice for conflicts_[arg]
        kKeepLocal,
        kKeepServer,
        kCancel,
        kRestoreSave, // arg: RomM's save id
        kRestoreState // arg: RomM's state id
    };
    struct Item
    {
        Row row;
        Do action = kNothing;
        int arg = 0;
    };
    struct Conflict
    {
        std::string path, game, local, server;
    };

    void load(App &app);
    void show_conflicts();
    void add(Row row, Do action = kNothing, int arg = 0) { items_.push_back({std::move(row), action, arg}); }
    std::vector<Row> rows() const;

    bool active_ = false;
    Kind kind_ = kActivity;
    int rom_id_ = 0;
    std::string name_;
    std::string title_, subtitle_;
    std::vector<Item> items_;
    std::vector<Conflict> conflicts_;
    int choosing_ = -1; // the conflict whose choice is showing, or -1
    int armed_ = -1;    // a restore row waiting for its second press
    float armed_age_ = 0.0f, age_ = 0.0f;
    int generation_ = 0; // answers for an older open() are dropped
    int waiting_ = 0;    // requests still out
    bool busy_ = false;  // a resolve or restore in flight
    // A game's view gathers three answers before it shows.
    std::string game_here_, game_history_, game_log_;
    RowList list_;
};

} // namespace rommps
