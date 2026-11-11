// MyOBS switcher core: Preview / Program, CUT and Fade on a frame boundary.
//
// Every input is a scene (an input can hold layers). The program output (channel 0) is one transition
// source. UI calls (Cut, Fade, SetPreview) only enqueue requests; they are executed in the engine's
// tick, before the frame is rendered, one request per frame. A click during a running fade is kept in
// the queue and executed when the fade ends - never lost, never mixed into the running transition.
#pragma once

#include <obs.h>

#include "audiobus.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace myobs {

class Switcher {
public:
	struct Event {
		enum class Kind { Cut, FadeStart, FadeEnd, Preview } kind;
		int program;
		int preview;
		uint64_t frame; // engine frame counter when executed
	};

	Switcher();
	~Switcher();
	Switcher(const Switcher &) = delete;
	Switcher &operator=(const Switcher &) = delete;

	// Wraps the source into a scene named like the input. Returns the input index.
	int AddInput(obs_source_t *source, const std::string &name);
	// Removes an input that is not on air and not in a running fade. Returns false if refused.
	bool RemoveInput(int index);
	int InputCount() const;
	std::string InputName(int index) const;
	obs_source_t *InputScene(int index) const; // not referenced; valid while the switcher lives

	// Sound of an input (see audiobus.hpp): «в эфир» and AFV. Inputs without sound ignore it.
	bool HasAudio(int index) const;
	void SetAudioOnAir(int index, bool onAir);
	void SetAudioAfv(int index, bool afv);
	bool AudioOnAir(int index) const;
	bool AudioAfv(int index) const;

	// Requests (any thread).
	void SetPreview(int index);
	void Cut();
	void Fade(uint32_t ms = 500);

	// State as of the last executed frame (any thread).
	int Program() const { return program_.load(); }
	int Preview() const { return preview_.load(); }
	bool Transitioning() const { return transitioning_.load(); }
	size_t PendingRequests() const;

	// Called on the engine thread after a request is executed (tests, UI refresh).
	void SetEventCallback(std::function<void(const Event &)> cb);

private:
	struct Request {
		enum class Kind { Preview, Cut, Fade } kind;
		int index;
		uint32_t ms;
	};
	static void TickCallback(void *data, float seconds);
	static void TransitionStopped(void *data, calldata_t *cd);
	void Tick();
	void Emit(Event::Kind kind);

	obs_source_t *transition_ = nullptr;
	obs_source_t *proxy_ = nullptr; // channel 0: picture of the transition, no sound tree
	std::unique_ptr<AudioBus> bus_;
	std::vector<obs_source_t *> sources_; // the inputs themselves (scenes hold the references)
	std::vector<obs_scene_t *> scenes_;
	std::vector<std::string> names_;
	mutable std::mutex mutex_;
	std::deque<Request> queue_;
	std::function<void(const Event &)> onEvent_;
	std::atomic<int> program_{-1};
	std::atomic<int> preview_{-1};
	std::atomic<bool> transitioning_{false};
	std::atomic<bool> stopped_{false};
	int fadeTarget_ = -1;
	uint64_t frame_ = 0;
};

} // namespace myobs
