#pragma once

// ASI loading.
//
// An `.asi` is a DLL with a different extension and no required exports: it
// does its work from DllMain, usually by starting a thread and installing
// hooks. That convention predates this project by twenty years and most of the
// Most Wanted mods people already have are written to it, so MWOn12 loads them
// as they are rather than asking anyone to rebuild.
//
// This is a separate thing from an MWOn12 *plugin* (see PluginHost.h). A plugin
// exports MWOn12PluginMain, gets the live D3D12 device once per frame, and is
// how you draw. An ASI gets neither and does not need them: it is for changing
// the game, not the picture. Both can be installed at once and neither knows
// about the other.
//
// See the [ASI] section of MWOn12.ini for where they are loaded from.

namespace mwon12::asi {

// Loads every .asi found, once per process. Safe to call more than once; the
// second call does nothing.
//
// Called from the first Direct3DCreate9 rather than from DllMain, deliberately.
// LoadLibrary inside DllMain runs under the loader lock, and an ASI that loads
// its own dependencies -- most of them do -- can deadlock there. The first
// create call is outside the lock and still well before the game has drawn
// anything, which is early enough for any hook an ASI wants to install.
void LoadAll();

// How many loaded. For the log line and for anything that wants to say so.
unsigned LoadedCount() noexcept;

}  // namespace mwon12::asi
