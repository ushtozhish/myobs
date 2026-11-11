/* Live thumbnails for the input tiles.
 *
 * After the program frame is rendered (obs_add_main_rendered_callback, engine graphics thread) every
 * input scene is drawn once into a small texture; each tile display only copies its texture. The
 * program frame always comes first, thumbnails use what is left: at more than 30 fps they are
 * refreshed every second frame. */
#pragma once

#include <obs.hpp>
#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <mutex>
#include <vector>

class Thumbs {
public:
	static constexpr uint32_t kW = 384, kH = 216;

	static Thumbs &Get()
	{
		static Thumbs t;
		return t;
	}

	void Start() { obs_add_main_rendered_callback(Rendered, this); }

	void Stop()
	{
		obs_remove_main_rendered_callback(Rendered, this);
		obs_enter_graphics();
		std::lock_guard<std::mutex> lock(mutex_);
		for (Entry &e : entries_)
			if (e.tr)
				gs_texrender_destroy(e.tr);
		for (gs_texrender_t *t : trash_)
			gs_texrender_destroy(t);
		entries_.clear();
		trash_.clear();
		obs_leave_graphics();
	}

	/* UI thread: the scenes that need thumbnails. */
	void SetScenes(const std::vector<obs_source_t *> &scenes)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<Entry> next;
		for (obs_source_t *s : scenes) {
			auto it = std::find_if(entries_.begin(), entries_.end(), [s](const Entry &e) { return e.key == s; });
			if (it != entries_.end()) {
				next.push_back(*it);
				it->tr = nullptr;
			} else {
				next.push_back({OBSGetWeakRef(s), s, nullptr, false});
			}
		}
		for (Entry &e : entries_)
			if (e.tr)
				trash_.push_back(e.tr);
		entries_ = std::move(next);
	}

	/* Graphics thread, inside a display draw with the canvas ortho set (base width x height). */
	void Draw(obs_source_t *scene, uint32_t baseW, uint32_t baseH)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = std::find_if(entries_.begin(), entries_.end(), [scene](const Entry &e) { return e.key == scene; });
		if (it == entries_.end() || !it->ready || !it->tr)
			return;
		gs_texture_t *tex = gs_texrender_get_texture(it->tr);
		if (!tex)
			return;
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_effect_set_texture_srgb(image, tex);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(tex, 0, baseW, baseH);
		gs_enable_framebuffer_srgb(previous);
	}

private:
	struct Entry {
		OBSWeakSource weak;
		obs_source_t *key;
		gs_texrender_t *tr;
		bool ready;
	};

	static void Rendered(void *param) { static_cast<Thumbs *>(param)->Render(); }

	void Render()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (gs_texrender_t *t : trash_)
			gs_texrender_destroy(t);
		trash_.clear();
		obs_video_info ovi = {};
		if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
			return;
		frame_++;
		if (ovi.fps_den && ovi.fps_num / ovi.fps_den > 30 && (frame_ & 1))
			return;
		const uint32_t w = ovi.base_width >= ovi.base_height ? kW : kH;
		const uint32_t h = ovi.base_width >= ovi.base_height ? kH : kW;
		for (Entry &e : entries_) {
			OBSSource s = OBSGetStrongRef(e.weak);
			if (!s)
				continue;
			if (!e.tr)
				e.tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			if (!e.tr)
				continue;
			gs_texrender_reset(e.tr);
			if (gs_texrender_begin(e.tr, w, h)) {
				vec4 clear;
				vec4_zero(&clear);
				gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
				gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
				gs_blend_state_push();
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
				obs_source_video_render(s);
				gs_blend_state_pop();
				gs_texrender_end(e.tr);
				e.ready = true;
			}
		}
	}

	std::mutex mutex_;
	std::vector<Entry> entries_;
	std::vector<gs_texrender_t *> trash_;
	uint64_t frame_ = 0;
};
