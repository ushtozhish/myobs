#include "recorder.hpp"

#include <util/platform.h>

#include <chrono>
#include <thread>

namespace myobs {

static bool Registered(const char *encoderId)
{
	return obs_encoder_get_display_name(encoderId) != nullptr;
}

bool Recorder::Start(const RecordSettings &s)
{
	Stop();
	error_.clear();
	if (s.path.empty()) {
		error_ = "no file path";
		return false;
	}

	/* video encoder */
	obs_data_t *vs = obs_data_create();
	obs_data_set_string(vs, "rate_control", "CBR");
	obs_data_set_int(vs, "bitrate", s.videoKbps);
	obs_data_set_string(vs, "profile", "high");
	const std::string keyint = std::to_string(s.keyframeEvery);
	if (s.preferNvenc && Registered("obs_nvenc_h264_tex")) {
		videoId_ = "obs_nvenc_h264_tex";
		obs_data_set_int(vs, "bf", 0);
		obs_data_set_string(vs, "preset2", "p5");
		obs_data_set_string(vs, "opts", ("keyint=" + keyint).c_str());
	} else {
		videoId_ = "obs_x264";
		obs_data_set_int(vs, "bf", 0);
		obs_data_set_string(vs, "preset", "veryfast");
		obs_data_set_string(vs, "x264opts",
				    ("keyint=" + keyint + " min-keyint=" + keyint + " bframes=0 scenecut=0").c_str());
	}
	video_ = obs_video_encoder_create(videoId_.c_str(), "MyOBS record video", vs, nullptr);
	obs_data_release(vs);
	if (!video_) {
		error_ = "video encoder " + videoId_ + " not available";
		Release();
		return false;
	}
	obs_encoder_set_video(video_, obs_get_video());

	/* file output */
	obs_data_t *os = obs_data_create();
	obs_data_set_string(os, "path", s.path.c_str());
	output_ = obs_output_create("ffmpeg_muxer", "MyOBS record", os, nullptr);
	obs_data_release(os);
	if (!output_) {
		error_ = "file output (obs-ffmpeg) not available";
		Release();
		return false;
	}
	obs_output_set_video_encoder(output_, video_);

	/* one AAC track per chosen bus */
	size_t track = 0;
	for (size_t bus = 0; bus < 6; bus++) {
		if (!(s.audioTracks & (1u << bus)))
			continue;
		obs_data_t *as = obs_data_create();
		obs_data_set_int(as, "bitrate", 320);
		audio_[track] = obs_audio_encoder_create("ffmpeg_aac", ("MyOBS record audio " + std::to_string(bus)).c_str(),
							 as, bus, nullptr);
		obs_data_release(as);
		if (!audio_[track]) {
			error_ = "audio encoder (ffmpeg_aac) not available";
			Release();
			return false;
		}
		obs_encoder_set_audio(audio_[track], obs_get_audio());
		obs_output_set_audio_encoder(output_, audio_[track], track);
		track++;
	}

	if (!obs_output_start(output_)) {
		const char *e = obs_output_get_last_error(output_);
		error_ = e ? e : "output did not start";
		Release();
		return false;
	}
	blog(LOG_INFO, "[MyOBS] recording to %s (%s, key frame every %u frames, %zu audio tracks)", s.path.c_str(),
	     videoId_.c_str(), s.keyframeEvery, track);
	return true;
}

void Recorder::Stop()
{
	if (output_ && obs_output_active(output_)) {
		obs_output_stop(output_);
		/* the muxer closes the file after the encoders drain */
		for (int i = 0; i < 300 && obs_output_active(output_); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	Release();
}

void Recorder::Release()
{
	if (output_)
		obs_output_release(output_);
	output_ = nullptr;
	if (video_)
		obs_encoder_release(video_);
	video_ = nullptr;
	for (auto &a : audio_) {
		if (a)
			obs_encoder_release(a);
		a = nullptr;
	}
}

} // namespace myobs
