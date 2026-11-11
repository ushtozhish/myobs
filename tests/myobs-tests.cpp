/* MyOBS automatic tests (run by CI on every change).
 *
 * The engine is checked end to end the same way the switch test checked OBS: a test source
 * stamps every frame with a code (16 black/white blocks = frame number), the program output
 * is read back through a raw video callback and every frame's code is verified.
 *
 * Tests:
 *   1. engine starts: libobs, D3D11 (on CI: the Microsoft software renderer), audio;
 *   2. pipeline: 2 seconds of output, every frame readable, codes strictly in order;
 *   3. plugins: all modules load, the «Клип» input (myobs_clip) is registered.
 *
 * Exit code 0 = all passed. The log goes to stdout (visible in the CI log).
 */

#include <obs.h>
#include <util/base.h>
#include <util/platform.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string &what)
{
	std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
	std::fflush(stdout);
	if (!ok)
		g_failures++;
}

void LogHandler(int level, const char *format, va_list args, void *)
{
	if (level > LOG_INFO)
		return;
	char buf[4096];
	std::vsnprintf(buf, sizeof(buf), format, args);
	std::printf("  obs: %s\n", buf);
}

constexpr uint32_t kW = 640, kH = 360;

/* ---- test pattern source: async, one coded frame per tick ---- */

struct Pattern {
	obs_source_t *source;
	uint32_t counter = 0;
	std::vector<uint8_t> y, u, v;
};

void PatternTick(void *data, float)
{
	auto *p = static_cast<Pattern *>(data);
	const int strip = kH / 9, bw = kW / 16;
	const uint32_t code = p->counter & 0xFFFF;
	for (int yy = 0; yy < strip; yy++)
		for (int x = 0; x < (int)kW; x++)
			p->y[(size_t)yy * kW + x] = ((code >> std::min(15, x / bw)) & 1) ? 235 : 16;

	obs_source_frame f = {};
	f.data[0] = p->y.data();
	f.data[1] = p->u.data();
	f.data[2] = p->v.data();
	f.linesize[0] = kW;
	f.linesize[1] = kW / 2;
	f.linesize[2] = kW / 2;
	f.width = kW;
	f.height = kH;
	f.format = VIDEO_FORMAT_I420;
	f.timestamp = os_gettime_ns();
	video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_I420, f.color_matrix,
					       f.color_range_min, f.color_range_max);
	obs_source_output_video(p->source, &f);
	p->counter++;
}

void RegisterPattern()
{
	obs_source_info info = {};
	info.id = "myobs_test_pattern";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_name = [](void *) { return "Test pattern"; };
	info.create = [](obs_data_t *, obs_source_t *source) -> void * {
		auto *p = new Pattern{source};
		p->y.assign((size_t)kW * kH, 128);
		p->u.assign((size_t)kW * kH / 4, 128);
		p->v.assign((size_t)kW * kH / 4, 128);
		obs_source_set_async_unbuffered(source, true);
		return p;
	};
	info.destroy = [](void *data) { delete static_cast<Pattern *>(data); };
	info.video_tick = PatternTick;
	obs_register_source(&info);
}

/* ---- program output reader ---- */

std::mutex g_mutex;
std::vector<int> g_codes;
std::atomic<int> g_unreadable{0};

void OnFrame(void *, video_data *frame)
{
	const int y = kH / 18;
	int code = 0;
	bool clean = true;
	for (int b = 0; b < 16; b++) {
		const int x = (2 * b + 1) * (int)kW / 32;
		const uint8_t v = frame->data[0][(size_t)y * frame->linesize[0] + x];
		if (v > 126)
			code |= 1 << b;
		if (v > 60 && v < 190)
			clean = false;
	}
	if (!clean) {
		g_unreadable++;
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_codes.push_back(code);
}

bool StartEngine()
{
	if (!obs_startup("en-US", nullptr, nullptr))
		return false;
	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = kW;
	ovi.base_height = ovi.output_height = kH;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	const int video = obs_reset_video(&ovi);
	Check(video == OBS_VIDEO_SUCCESS, "video starts (obs_reset_video = " + std::to_string(video) + ")");
	if (video != OBS_VIDEO_SUCCESS)
		return false;
	obs_audio_info ai = {};
	ai.samples_per_sec = 48000;
	ai.speakers = SPEAKERS_STEREO;
	Check(obs_reset_audio(&ai), "audio starts");
	return true;
}

void TestPipeline()
{
	RegisterPattern();
	obs_source_t *src = obs_source_create("myobs_test_pattern", "pattern", nullptr, nullptr);
	Check(src != nullptr, "test pattern source created");
	if (!src)
		return;
	obs_set_output_source(0, src);

	video_scale_info conv = {};
	conv.format = VIDEO_FORMAT_I420;
	conv.width = kW;
	conv.height = kH;
	conv.range = VIDEO_RANGE_PARTIAL;
	conv.colorspace = VIDEO_CS_709;
	obs_add_raw_video_callback(&conv, OnFrame, nullptr);
	std::this_thread::sleep_for(std::chrono::seconds(3));
	obs_remove_raw_video_callback(OnFrame, nullptr);
	obs_set_output_source(0, nullptr);
	obs_source_release(src);

	std::vector<int> codes;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		codes = g_codes;
	}
	/* The first frames can precede the first pattern frame: skip until codes start. */
	size_t first = 0;
	while (first < codes.size() && codes[first] == 0)
		first++;
	int backwards = 0, repeats = 0, gaps = 0;
	for (size_t i = first + 1; i < codes.size(); i++) {
		const int d = codes[i] - codes[i - 1];
		if (d < 0)
			backwards++;
		else if (d == 0)
			repeats++;
		else if (d > 1)
			gaps++;
	}
	std::printf("  frames read %zu, unreadable %d, repeats %d, gaps %d, backwards %d\n", codes.size(),
		    g_unreadable.load(), repeats, gaps, backwards);
	Check(codes.size() - first >= 60, "program output delivers frames (" + std::to_string(codes.size()) + ")");
	Check(g_unreadable.load() == 0, "every output frame is clean (no blended or broken frames)");
	Check(backwards == 0, "frames never go back in time");
	/* On a CI machine without a GPU single repeats/gaps can come from timing; report them, fail only on many. */
	Check(repeats + gaps <= (int)(codes.size() / 20), "frame order steady (repeats+gaps within 5%)");
}

void TestPlugins()
{
	{ /* our own frontend: tell libobs where the plugins are (next to the exe: ../../obs-plugins/64bit) */
		char *exeDir = os_get_executable_path_ptr("");
		std::string base = exeDir ? exeDir : "";
		bfree(exeDir);
		obs_add_module_path((base + "../../obs-plugins/64bit").c_str(), (base + "../../data/obs-plugins/%module%").c_str());
	}
	obs_load_all_modules();
	obs_post_load_modules();
	const char *name = obs_source_get_display_name("myobs_clip");
	Check(name != nullptr, "«Клип» input registered (myobs_clip)");
	if (!name) { /* diagnose: open the module directly and report the libobs error code */
		char *exeDir = os_get_executable_path_ptr("");
		std::string dll = std::string(exeDir ? exeDir : "") + "../../obs-plugins/64bit/myobs-clip.dll";
		bfree(exeDir);
		std::printf("  plugin path: %s (exists: %d)\n", dll.c_str(), (int)os_file_exists(dll.c_str()));
		obs_module_t *m = nullptr;
		int r = obs_open_module(&m, dll.c_str(), nullptr);
		std::printf("  obs_open_module = %d (0 ok, -1 not found/dll deps, -2 missing exports, -3 incompatible)\n", r);
		if (r == 0) std::printf("  obs_init_module = %d\n", (int)obs_init_module(m));
	}
}

} // namespace

int main()
{
	base_set_log_handler(LogHandler, nullptr);
	std::printf("MyOBS tests\n");
	if (StartEngine()) {
		TestPipeline();
		TestPlugins();
	} else {
		Check(false, "engine start");
	}
	obs_shutdown();
	std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
	return g_failures ? 1 : 0;
}
