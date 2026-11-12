// MyOBS triggers: actions that run by themselves when an input goes on or off air.
//
// Event of an input: it goes on air (OnAir) or off air (OffAir) - at the CUT frame, or at the
// start of a FADE, so automation runs together with the picture.
// Actions (each with «через» = delay before it starts):
//   Volume     - automation level of an input (this one or any other): off, a level in dB, or
//                «как было» (back to the fader), reached «за» rampSec; «из тишины» starts from 0.
//                The user's fader is never touched: «как было» returns exactly to it.
//   GoOnAir    - put an input on air (CUT, or FADE of transitionMs).
//   ShowScreen - show an input on a screen output (handled by the program, see Switcher).
#pragma once

#include <obs.h>

#include <string>
#include <vector>

namespace myobs {

enum class TriggerEvent { OnAir, OffAir };

struct TriggerAction {
	enum class Kind { Volume, GoOnAir, ShowScreen } kind = Kind::Volume;
	obs_source_t *target = nullptr; // input acted on; nullptr = the trigger's own input
	enum class Level { Off, Db, Restore } level = Level::Off;
	double db = 0.0;
	bool fromSilence = false;
	double rampSec = 1.0;
	double delaySec = 0.0;
	uint32_t transitionMs = 0; // GoOnAir: 0 = CUT
	std::string screen;        // ShowScreen
};

struct Trigger {
	TriggerEvent event = TriggerEvent::OnAir;
	std::vector<TriggerAction> actions;
};

} // namespace myobs
