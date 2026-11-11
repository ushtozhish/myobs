// MyOBS program picture: our own transition source in output channel 0.
//
// Holds the input on air (A) and, during a FADE, the incoming one (B); blends them on the GPU.
// Everything happens on the engine video thread: the switcher changes the state in its tick and the
// source draws it in the same frame, so a change is frame-exact. The fade advances by frames.
// The source enumerates no children: the picture chain adds no sound to the output (sound goes
// through AudioBus). Inputs on air are activated explicitly as its active children.
#pragma once

#include <obs.h>

namespace myobs {

class ProgramMix {
public:
	ProgramMix();  // creates the source and puts it into output channel 0
	~ProgramMix(); // takes it out

	// Engine video thread (switcher tick).
	void Cut(obs_source_t *scene);
	void StartFade(obs_source_t *scene, uint32_t frames);
	bool Advance(); // once per frame; true when a running fade has just finished (B is now A)
	bool Fading() const { return b_ != nullptr; }

	// Called by the source (video thread).
	void Render();

private:
	void SetA(obs_source_t *scene);
	void Activate(obs_source_t *scene);
	void Deactivate(obs_source_t *scene);

	obs_source_t *self_ = nullptr;
	obs_source_t *a_ = nullptr; // referenced
	obs_source_t *b_ = nullptr; // referenced
	float t_ = 0.0f;
	float step_ = 0.0f;
	gs_texrender_t *texA_ = nullptr;
	gs_texrender_t *texB_ = nullptr;
	gs_effect_t *effect_ = nullptr;
};

} // namespace myobs
