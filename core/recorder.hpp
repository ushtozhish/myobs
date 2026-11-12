// Recording of the program in the working codec.
//
// Video: H.264, 4:2:0, a key frame every 10 frames, no B-frames, constant 40 Mbit/s - instant
// seeking in editing, hardware decoding everywhere. NVIDIA NVENC when present, x264 otherwise.
// Sound: track 1 = bus «Мастер-стрим», tracks 2.. = buses A, B (as chosen), AAC 320 kbit/s.
// Container: MKV by default (a recording cut by a crash stays readable).
#pragma once

#include <obs.h>

#include <atomic>
#include <string>

namespace myobs {

struct RecordSettings {
	std::string path;          // file to write (.mkv, .mov, .mp4)
	uint32_t videoKbps = 40000;
	uint32_t keyframeEvery = 10; // frames
	uint32_t audioTracks = 0x1;  // bit per bus: bit 0 Мастер-стрим, bit 1 A, bit 2 B
	bool preferNvenc = true;
};

class Recorder {
public:
	Recorder() = default;
	~Recorder() { Stop(); }
	Recorder(const Recorder &) = delete;
	Recorder &operator=(const Recorder &) = delete;

	bool Start(const RecordSettings &s); // false: see Error()
	void Stop();                         // waits until the file is closed
	bool Active() const { return output_ && obs_output_active(output_); }
	std::string Error() const { return error_; }
	std::string VideoEncoderId() const { return videoId_; }

private:
	void Release();
	obs_output_t *output_ = nullptr;
	obs_encoder_t *video_ = nullptr;
	obs_encoder_t *audio_[6] = {};
	std::string error_;
	std::string videoId_;
};

} // namespace myobs
