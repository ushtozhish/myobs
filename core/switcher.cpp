#include "switcher.hpp"

namespace myobs {

Switcher::Switcher()
{
	// The Fade transition (obs-transitions plugin) also performs CUT: obs_transition_set swaps instantly.
	transition_ = obs_source_create_private("fade_transition", "MyOBS program", nullptr);
	if (transition_) {
		signal_handler_connect(obs_source_get_signal_handler(transition_), "transition_stop",
				       TransitionStopped, this);
		obs_set_output_source(0, transition_);
	}
	obs_add_tick_callback(TickCallback, this);
}

Switcher::~Switcher()
{
	obs_remove_tick_callback(TickCallback, this);
	obs_set_output_source(0, nullptr);
	if (transition_) {
		signal_handler_disconnect(obs_source_get_signal_handler(transition_), "transition_stop",
					  TransitionStopped, this);
		obs_source_release(transition_);
	}
	for (obs_scene_t *s : scenes_) {
		obs_source_dec_showing(obs_scene_get_source(s));
		obs_scene_release(s);
	}
}

int Switcher::AddInput(obs_source_t *source, const std::string &name)
{
	obs_scene_t *scene = obs_scene_create_private(name.c_str());
	obs_sceneitem_t *item = obs_scene_add(scene, source);
	// Fit the input to the canvas.
	obs_video_info ovi = {};
	if (item && obs_get_video_info(&ovi)) {
		obs_transform_info t = {};
		obs_sceneitem_get_info2(item, &t);
		t.bounds_type = OBS_BOUNDS_SCALE_INNER;
		t.bounds_alignment = OBS_ALIGN_CENTER;
		t.bounds.x = (float)ovi.base_width;
		t.bounds.y = (float)ovi.base_height;
		t.pos.x = t.pos.y = 0;
		obs_sceneitem_set_info2(item, &t);
	}
	// Every input is shown all the time (tiles / multiview), so async inputs always hold a current frame
	// and a CUT never meets an empty input.
	obs_source_inc_showing(obs_scene_get_source(scene));
	std::lock_guard<std::mutex> lock(mutex_);
	scenes_.push_back(scene);
	names_.push_back(name);
	const int index = (int)scenes_.size() - 1;
	if (index == 0) {
		queue_.push_back({Request::Kind::Preview, 0, 0});
		queue_.push_back({Request::Kind::Cut, 0, 0}); // first input goes to program
	}
	return index;
}

bool Switcher::RemoveInput(int index)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (index < 0 || index >= (int)scenes_.size() || index == program_ || index == fadeTarget_)
		return false;
	obs_scene_t *scene = scenes_[index];
	scenes_.erase(scenes_.begin() + index);
	names_.erase(names_.begin() + index);
	auto shift = [index](int i) { return i > index ? i - 1 : i; };
	program_ = shift(program_);
	preview_ = preview_ == index ? program_.load() : shift(preview_);
	fadeTarget_ = shift(fadeTarget_);
	for (auto it = queue_.begin(); it != queue_.end();) {
		if (it->kind == Request::Kind::Preview && it->index == index) {
			it = queue_.erase(it);
			continue;
		}
		if (it->kind == Request::Kind::Preview)
			it->index = shift(it->index);
		++it;
	}
	obs_source_dec_showing(obs_scene_get_source(scene));
	obs_scene_release(scene);
	return true;
}

int Switcher::InputCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return (int)scenes_.size();
}

std::string Switcher::InputName(int index) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return index >= 0 && index < (int)names_.size() ? names_[index] : std::string();
}

obs_source_t *Switcher::InputScene(int index) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return index >= 0 && index < (int)scenes_.size() ? obs_scene_get_source(scenes_[index]) : nullptr;
}

void Switcher::SetPreview(int index)
{
	std::lock_guard<std::mutex> lock(mutex_);
	queue_.push_back({Request::Kind::Preview, index, 0});
}

void Switcher::Cut()
{
	std::lock_guard<std::mutex> lock(mutex_);
	queue_.push_back({Request::Kind::Cut, -1, 0});
}

void Switcher::Fade(uint32_t ms)
{
	std::lock_guard<std::mutex> lock(mutex_);
	queue_.push_back({Request::Kind::Fade, -1, ms});
}

size_t Switcher::PendingRequests() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return queue_.size();
}

void Switcher::SetEventCallback(std::function<void(const Event &)> cb)
{
	std::lock_guard<std::mutex> lock(mutex_);
	onEvent_ = std::move(cb);
}

void Switcher::TickCallback(void *data, float)
{
	static_cast<Switcher *>(data)->Tick();
}

void Switcher::TransitionStopped(void *data, calldata_t *)
{
	static_cast<Switcher *>(data)->stopped_ = true;
}

void Switcher::Emit(Event::Kind kind)
{
	if (onEvent_)
		onEvent_({kind, program_.load(), preview_.load(), frame_});
}

// Engine thread, once per frame, before the frame is rendered.
void Switcher::Tick()
{
	std::lock_guard<std::mutex> lock(mutex_);
	frame_++;
	if (!transition_)
		return;

	if (transitioning_ && stopped_) {
		// Fade finished: the former preview is on air, the former program goes to preview (vMix).
		transitioning_ = false;
		stopped_ = false;
		const int old = program_;
		program_ = fadeTarget_;
		preview_ = old;
		fadeTarget_ = -1;
		Emit(Event::Kind::FadeEnd);
		return; // the fade end takes this frame; the next request runs on the next one
	}

	// Preview changes are allowed during a fade only for inputs not involved in it.
	while (!queue_.empty()) {
		Request r = queue_.front();
		if (r.kind == Request::Kind::Preview) {
			if (r.index < 0 || r.index >= (int)scenes_.size()) {
				queue_.pop_front();
				continue;
			}
			if (transitioning_)
				break; // keep order: preview after the queued fade/cut
			queue_.pop_front();
			preview_ = r.index;
			Emit(Event::Kind::Preview);
			continue; // preview is not visible on air, it does not use up the frame
		}
		if (transitioning_)
			break;
		queue_.pop_front();
		const int target = preview_;
		if (target < 0 || target >= (int)scenes_.size())
			continue;
		obs_source_t *scene = obs_scene_get_source(scenes_[target]);
		if (r.kind == Request::Kind::Cut || program_ < 0) {
			obs_transition_set(transition_, scene);
			const int old = program_;
			program_ = target;
			preview_ = old >= 0 ? old : target;
			Emit(Event::Kind::Cut);
		} else {
			fadeTarget_ = target;
			stopped_ = false;
			transitioning_ = true;
			if (!obs_transition_start(transition_, OBS_TRANSITION_MODE_AUTO, r.ms, scene)) {
				// Could not start (should not happen): fall back to a clean cut.
				transitioning_ = false;
				obs_transition_set(transition_, scene);
				const int old = program_;
				program_ = target;
				preview_ = old;
				Emit(Event::Kind::Cut);
			} else {
				Emit(Event::Kind::FadeStart);
			}
		}
		break; // one on-air change per frame
	}
}

} // namespace myobs
