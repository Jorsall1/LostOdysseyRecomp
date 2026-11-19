#pragma once
// The host extras on the idle title menu (settings/title_entry.cpp): Y opens
// Settings, Back opens a Quit confirmation, and the footer legend shows both.
// Switch 2 (turn 35, 2026-11-18) keeps Y: Settings and drops the quit: the
// game is closed with HOME -> X, so Back does nothing extra and the footer
// shows only the Settings key (and the credit).
namespace settings::title_policy
{
#if defined(__SWITCH2__)
inline constexpr bool kDefaultQuit = false;
#else
inline constexpr bool kDefaultQuit = true;
#endif

struct Policy
{
    bool settings = true;
    bool quit = kDefaultQuit;
};

struct Tick
{
    bool quitIdle = false;     // passed to UpdateTitleQuit: false never opens the dialog and drops a Back press
    bool settingsIdle = false; // passed to ConsumeTitleShortcut: arms Y
    bool hint = false;         // ReportHint: the footer legend
};

// menuIdle: the title menu waits for input (MenuIdle). quitOwnsInput: the
// Quit dialog took this tick's input (UpdateTitleQuit's result); it can only be
// true when quitIdle was.
inline constexpr bool QuitIdle(Policy policy, bool menuIdle) { return policy.quit && menuIdle; }
inline constexpr Tick Decide(Policy policy, bool menuIdle, bool quitOwnsInput)
{
    Tick tick;
    tick.quitIdle = QuitIdle(policy, menuIdle);
    const bool free = menuIdle && !(tick.quitIdle && quitOwnsInput);
    tick.settingsIdle = policy.settings && free;
    tick.hint = (policy.settings || policy.quit) && free;
    return tick;
}
// The footer's quit legend (MenuSnapshot::titleQuitKey / titleQuitLabel stay empty without it).
inline constexpr bool ShowQuitLegend(Policy policy) { return policy.quit; }
}
