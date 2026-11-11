/* MyOBS «NDI» input.
 *
 * Receives an NDI source through the NDI Runtime installed on the machine (loaded at run time,
 * nothing from NDI is shipped with MyOBS). Uses the NDI frame synchroniser: on every engine tick
 * the input takes the current video frame and exactly one tick of audio, resampled to the engine
 * rate, so picture and sound follow the program clock without drift or growing delay. */

#include <obs-module.h>
#include <util/platform.h>

#include <Processing.NDI.Lib.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

OBS_DECLARE_MODULE()

namespace {

const NDIlib_v6 *g_ndi = nullptr;
NDIlib_find_instance_t g_finder = nullptr;
std::mutex g_finderMutex;

const NDIlib_v6 *LoadNdi()
{
#ifdef _WIN32
	HMODULE dll = nullptr;
	char dir[MAX_PATH] = {};
	if (GetEnvironmentVariableA(NDILIB_REDIST_FOLDER, dir, MAX_PATH)) {
		const std::string path = std::string(dir) + "\\" + NDILIB_LIBRARY_NAME;
		dll = LoadLibraryA(path.c_str());
	}
	if (!dll)
		dll = LoadLibraryA(NDILIB_LIBRARY_NAME);
	if (!dll) {
		blog(LOG_INFO, "[MyOBS NDI] NDI Runtime not found (install NDI Tools or NDI Runtime)");
		return nullptr;
	}
	using LoadFn = const NDIlib_v6 *(*)(void);
	auto load = (LoadFn)GetProcAddress(dll, "NDIlib_v6_load");
	if (!load)
		load = (LoadFn)GetProcAddress(dll, "NDIlib_v5_load");
	const NDIlib_v6 *lib = load ? load() : nullptr;
	if (!lib || !lib->initialize()) {
		blog(LOG_WARNING, "[MyOBS NDI] NDI Runtime could not be initialised (CPU without SSE4.2?)");
		return nullptr;
	}
	blog(LOG_INFO, "[MyOBS NDI] NDI Runtime %s", lib->version ? lib->version() : "?");
	return lib;
#else
	return nullptr;
#endif
}

NDIlib_find_instance_t Finder()
{
	std::lock_guard<std::mutex> lock(g_finderMutex);
	if (!g_finder && g_ndi) {
		NDIlib_find_create_t c;
		c.show_local_sources = true;
		g_finder = g_ndi->find_create_v2(&c);
	}
	return g_finder;
}

class NdiInput {
public:
	explicit NdiInput(obs_source_t *source) : source_(source) { obs_source_set_async_unbuffered(source, true); }
	~NdiInput() { Disconnect(); }

	void Update(obs_data_t *settings)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		name_ = obs_data_get_string(settings, "ndi_name");
		lowBandwidth_ = obs_data_get_bool(settings, "low_bandwidth");
		audio_ = obs_data_get_bool(settings, "audio");
		DisconnectLocked();
		if (active_)
			ConnectLocked();
	}

	void Activate()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		active_ = true;
		ConnectLocked();
	}

	void Deactivate()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		active_ = false;
		DisconnectLocked();
		obs_source_output_video(source_, nullptr);
	}

	void Disconnect()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		DisconnectLocked();
	}

	/* Engine thread, once per frame. */
	void Tick(float seconds)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!fs_)
			return;
		NDIlib_video_frame_v2_t vf;
		g_ndi->framesync_capture_video(fs_, &vf, NDIlib_frame_format_type_progressive);
		if (vf.p_data && vf.timestamp != lastVideoTs_) {
			lastVideoTs_ = vf.timestamp;
			OutputVideo(vf);
		}
		g_ndi->framesync_free_video(fs_, &vf);

		if (!audio_)
			return;
		/* exactly one tick of audio, carrying the fraction over */
		audioDebt_ += seconds * 48000.0;
		const int samples = (int)audioDebt_;
		if (samples <= 0)
			return;
		audioDebt_ -= samples;
		NDIlib_audio_frame_v2_t af;
		g_ndi->framesync_capture_audio(fs_, &af, 48000, 2, samples);
		if (af.p_data && af.no_samples > 0) {
			obs_source_audio a = {};
			a.speakers = SPEAKERS_STEREO;
			a.format = AUDIO_FORMAT_FLOAT_PLANAR;
			a.samples_per_sec = 48000;
			a.frames = (uint32_t)af.no_samples;
			a.timestamp = os_gettime_ns();
			for (int c = 0; c < 2 && c < af.no_channels; c++)
				a.data[c] = (const uint8_t *)((const uint8_t *)af.p_data + (size_t)c * af.channel_stride_in_bytes);
			obs_source_output_audio(source_, &a);
		}
		g_ndi->framesync_free_audio(fs_, &af);
	}

private:
	void OutputVideo(const NDIlib_video_frame_v2_t &vf)
	{
		obs_source_frame f = {};
		f.width = (uint32_t)vf.xres;
		f.height = (uint32_t)vf.yres;
		f.timestamp = os_gettime_ns();
		f.data[0] = vf.p_data;
		f.linesize[0] = (uint32_t)vf.line_stride_in_bytes;
		video_range_type range = VIDEO_RANGE_PARTIAL;
		switch (vf.FourCC) {
		case NDIlib_FourCC_video_type_UYVY:
		case NDIlib_FourCC_video_type_UYVA: /* alpha plane follows, not used */
			f.format = VIDEO_FORMAT_UYVY;
			break;
		case NDIlib_FourCC_video_type_BGRA:
			f.format = VIDEO_FORMAT_BGRA;
			range = VIDEO_RANGE_FULL;
			break;
		case NDIlib_FourCC_video_type_BGRX:
			f.format = VIDEO_FORMAT_BGRX;
			range = VIDEO_RANGE_FULL;
			break;
		default:
			return;
		}
		const video_colorspace cs = vf.yres >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
		video_format_get_parameters_for_format(cs, range, f.format, f.color_matrix, f.color_range_min,
						       f.color_range_max);
		f.full_range = range == VIDEO_RANGE_FULL;
		obs_source_output_video(source_, &f);
	}

	void ConnectLocked()
	{
		if (!g_ndi || name_.empty() || recv_)
			return;
		NDIlib_recv_create_v3_t c;
		c.source_to_connect_to.p_ndi_name = name_.c_str();
		c.color_format = NDIlib_recv_color_format_UYVY_BGRA;
		c.bandwidth = lowBandwidth_ ? NDIlib_recv_bandwidth_lowest : NDIlib_recv_bandwidth_highest;
		c.allow_video_fields = false;
		c.p_ndi_recv_name = "MyOBS";
		recv_ = g_ndi->recv_create_v3(&c);
		if (recv_)
			fs_ = g_ndi->framesync_create(recv_);
		audioDebt_ = 0.0;
		lastVideoTs_ = -1;
		blog(LOG_INFO, "[MyOBS NDI] '%s': connect to '%s' %s", obs_source_get_name(source_), name_.c_str(),
		     fs_ ? "ok" : "failed");
	}

	void DisconnectLocked()
	{
		if (fs_)
			g_ndi->framesync_destroy(fs_);
		if (recv_)
			g_ndi->recv_destroy(recv_);
		fs_ = nullptr;
		recv_ = nullptr;
	}

	obs_source_t *source_;
	std::mutex mutex_;
	std::string name_;
	bool lowBandwidth_ = false;
	bool audio_ = true;
	bool active_ = false;
	NDIlib_recv_instance_t recv_ = nullptr;
	NDIlib_framesync_instance_t fs_ = nullptr;
	double audioDebt_ = 0.0;
	int64_t lastVideoTs_ = -1;
};

bool RefreshClicked(obs_properties_t *, obs_property_t *, void *)
{
	if (NDIlib_find_instance_t f = Finder())
		g_ndi->find_wait_for_sources(f, 1500);
	return true; /* rebuild the dialog with the new list */
}

obs_properties_t *GetProperties()
{
	obs_properties_t *props = obs_properties_create();
	if (!g_ndi) {
		obs_properties_add_text(props, "info_text",
					"NDI Runtime не найден. Установите NDI Tools или NDI Runtime (ndi.video) и перезапустите MyOBS.",
					OBS_TEXT_INFO);
		return props;
	}
	obs_property_t *list =
		obs_properties_add_list(props, "ndi_name", "Источник NDI", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	NDIlib_find_instance_t f = Finder();
	uint32_t n = 0;
	const NDIlib_source_t *sources = f ? g_ndi->find_get_current_sources(f, &n) : nullptr;
	if (f && n == 0) {
		g_ndi->find_wait_for_sources(f, 1000);
		sources = g_ndi->find_get_current_sources(f, &n);
	}
	std::vector<std::string> names;
	for (uint32_t i = 0; i < n; i++)
		names.push_back(sources[i].p_ndi_name ? sources[i].p_ndi_name : "");
	for (const std::string &name : names)
		obs_property_list_add_string(list, name.c_str(), name.c_str());
	obs_properties_add_button2(props, "refresh", "Обновить список источников", RefreshClicked, nullptr);
	obs_properties_add_bool(props, "low_bandwidth", "Низкое качество (меньше нагрузка на сеть)");
	obs_properties_add_bool(props, "audio", "Звук");
	return props;
}

} // namespace

bool obs_module_load(void)
{
	g_ndi = LoadNdi();
	obs_source_info info = {};
	info.id = "myobs_ndi";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = [](void *) { return "NDI"; };
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *in = new NdiInput(source);
		in->Update(settings);
		return in;
	};
	info.destroy = [](void *data) { delete static_cast<NdiInput *>(data); };
	info.update = [](void *data, obs_data_t *settings) { static_cast<NdiInput *>(data)->Update(settings); };
	/* connected while shown anywhere (MyOBS keeps every input shown), not only on air: a CUT never waits */
	info.show = [](void *data) { static_cast<NdiInput *>(data)->Activate(); };
	info.hide = [](void *data) { static_cast<NdiInput *>(data)->Deactivate(); };
	info.video_tick = [](void *data, float seconds) { static_cast<NdiInput *>(data)->Tick(seconds); };
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_bool(settings, "audio", true);
		obs_data_set_default_bool(settings, "low_bandwidth", false);
	};
	info.get_properties = [](void *) { return GetProperties(); };
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	obs_register_source(&info);
	return true;
}

void obs_module_unload(void)
{
	std::lock_guard<std::mutex> lock(g_finderMutex);
	if (g_finder && g_ndi)
		g_ndi->find_destroy(g_finder);
	g_finder = nullptr;
	if (g_ndi)
		g_ndi->destroy();
	g_ndi = nullptr;
}
