// RommPS: the Settings tab, in sections: when and what it syncs, downloads,
// the look and notifications, emulators, updates and the RomM connection; an
// About panel beside them.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "credits.hpp"
#include "rommps_app.hpp"
#include "rows.hpp"

namespace rommps
{

class SettingsPage
{
  public:
    void enter(App &app);
    void update(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app, float clock,
              float age) const;
    // What Cross does on the focused row, for the hints.
    const char *cross_hint(const App &app) const;
    // True once after Emulators was chosen: the screen opens that setup step.
    bool take_emulators_request()
    {
        const bool asked = emulators_asked_;
        emulators_asked_ = false;
        return asked;
    }
    // The Codec call behind the author's portrait; the screen draws it over everything.
    CodecCall &call() { return call_; }
    const CodecCall &call() const { return call_; }

  private:
    enum Id
    {
        kSaveSync,
        kLook,
        kSounds,
        kInterval,
        kOnExit,
        kOnStart,
        kOnChange,
        kStates,
        kPolicy,
        kConcurrency,
        kNotify,
        kUpdateCheck,
        kCovers,
        kUpdate,
        kEmulators,
        kSetup,
        kForget,
        kDiagnostics,
        kCount,
        kHeader = -1 // a section's name
    };
    // The rows as shown, sections included; ids[i] is row i's Id.
    std::vector<Row> rows(const App &app, std::vector<int> *ids = nullptr) const;

    RowList list_;
    bool on_credits_ = false;
    bool emulators_asked_ = false; // the focus is on the author's portrait, below the rows
    CodecCall call_;
    // The diagnostic bundle: 0 not asked, 1 being made, 2 saved (where_), 3 failed.
    int diag_ = 0;
    std::string diag_where_;
    int armed_ = -1; // a row that does something lasting, waiting for its second press
    float armed_age_ = 0.0f;
};

} // namespace rommps
