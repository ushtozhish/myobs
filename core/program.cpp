#include "program.hpp"

#include <graphics/graphics.h>
#include <graphics/vec4.h>

namespace myobs {
namespace {

const char *kFadeEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d tex_a;
uniform texture2d tex_b;
uniform float fade_val;

sampler_state textureSampler {
	Filter    = Linear;
	AddressU  = Clamp;
	AddressV  = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData VSDefault(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = v_in.uv;
	return vert_out;
}

float4 PSFade(VertData v_in) : TARGET
{
	float4 a = tex_a.Sample(textureSampler, v_in.uv);
	float4 b = tex_b.Sample(textureSampler, v_in.uv);
	return lerp(a, b, fade_val);
}

technique Fade
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSFade(v_in);
	}
}
)";

void Register()
{
	static bool done = false;
	if (done)
		return;
	done = true;
	obs_source_info info = {};
	info.id = "myobs_program";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
	info.get_name = [](void *) { return "MyOBS program"; };
	info.create = [](obs_data_t *settings, obs_source_t *) -> void * {
		return reinterpret_cast<void *>((uintptr_t)obs_data_get_int(settings, "mix"));
	};
	info.destroy = [](void *) {};
	info.get_width = [](void *) -> uint32_t {
		obs_video_info ovi = {};
		return obs_get_video_info(&ovi) ? ovi.base_width : 0;
	};
	info.get_height = [](void *) -> uint32_t {
		obs_video_info ovi = {};
		return obs_get_video_info(&ovi) ? ovi.base_height : 0;
	};
	info.video_render = [](void *data, gs_effect_t *) {
		if (data)
			static_cast<ProgramMix *>(data)->Render();
	};
	obs_register_source(&info);
}

bool RenderInto(gs_texrender_t *tr, obs_source_t *scene, uint32_t w, uint32_t h)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_video_render(scene);
	gs_blend_state_pop();
	gs_texrender_end(tr);
	return true;
}

} // namespace

ProgramMix::ProgramMix()
{
	Register();
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "mix", (long long)(uintptr_t)this);
	self_ = obs_source_create_private("myobs_program", "MyOBS program", s);
	obs_data_release(s);
	if (self_)
		obs_set_output_source(0, self_);
}

ProgramMix::~ProgramMix()
{
	if (self_)
		obs_set_output_source(0, nullptr); /* the video thread no longer renders it after this */
	obs_enter_graphics();
	if (texA_)
		gs_texrender_destroy(texA_);
	if (texB_)
		gs_texrender_destroy(texB_);
	if (effect_)
		gs_effect_destroy(effect_);
	obs_leave_graphics();
	Deactivate(a_);
	Deactivate(b_);
	obs_source_release(a_);
	obs_source_release(b_);
	obs_source_release(self_);
}

void ProgramMix::Activate(obs_source_t *scene)
{
	if (scene && self_)
		obs_source_add_active_child(self_, scene);
}

void ProgramMix::Deactivate(obs_source_t *scene)
{
	if (scene && self_)
		obs_source_remove_active_child(self_, scene);
}

void ProgramMix::SetA(obs_source_t *scene)
{
	if (scene == a_)
		return;
	obs_source_t *old = a_;
	a_ = obs_source_get_ref(scene);
	Activate(a_); /* new one first: an input in both keeps running */
	Deactivate(old);
	obs_source_release(old);
}

void ProgramMix::Cut(obs_source_t *scene)
{
	if (b_) { /* cut during a fade (not used by the switcher, which queues it) */
		Deactivate(b_);
		obs_source_release(b_);
		b_ = nullptr;
	}
	SetA(scene);
}

void ProgramMix::StartFade(obs_source_t *scene, uint32_t frames)
{
	if (!a_ || scene == a_ || frames == 0) {
		Cut(scene);
		return;
	}
	if (b_) {
		Deactivate(b_);
		obs_source_release(b_);
	}
	b_ = obs_source_get_ref(scene);
	Activate(b_);
	t_ = 0.0f;
	step_ = 1.0f / (float)frames;
}

bool ProgramMix::Advance()
{
	if (!b_)
		return false;
	t_ += step_;
	if (t_ < 1.0f - 1e-4f)
		return false;
	/* fade done: B becomes A (B is already active; drop the old A) */
	obs_source_t *old = a_;
	a_ = b_;
	b_ = nullptr;
	Deactivate(old);
	obs_source_release(old);
	t_ = 0.0f;
	return true;
}

void ProgramMix::Render()
{
	if (!b_) {
		if (a_)
			obs_source_video_render(a_);
		return;
	}
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return;
	const uint32_t w = ovi.base_width, h = ovi.base_height;
	if (!texA_)
		texA_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!texB_)
		texB_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!effect_) {
		char *err = nullptr;
		effect_ = gs_effect_create(kFadeEffect, "myobs_fade.effect", &err);
		if (err) {
			blog(LOG_ERROR, "[MyOBS] fade effect: %s", err);
			bfree(err);
		}
	}
	if (!texA_ || !texB_ || !effect_) {
		obs_source_video_render(t_ < 0.5f ? (a_ ? a_ : b_) : b_);
		return;
	}
	if (a_)
		RenderInto(texA_, a_, w, h);
	RenderInto(texB_, b_, w, h);
	gs_texture_t *ta = a_ ? gs_texrender_get_texture(texA_) : nullptr;
	gs_texture_t *tb = gs_texrender_get_texture(texB_);
	if (!tb)
		return;
	if (!ta)
		ta = tb;
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect_, "tex_a"), ta);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect_, "tex_b"), tb);
	gs_effect_set_float(gs_effect_get_param_by_name(effect_, "fade_val"), t_);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(effect_, "Fade"))
		gs_draw_sprite(ta, 0, w, h);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

} // namespace myobs
