/* MyOBS «Клип» — a media input built like an editing timeline, not like a player.
 *
 * OBS's media source "pushes": a thread decodes and queues timestamped frames, and OBS
 * looks for a matching one on every tick. On start, seek or take the queue is empty or
 * holds an old frame — the micro-flash measured by the MyOBS switch test.
 *
 * This input "pulls": every video tick computes the clip frame NUMBER that must be on
 * screen (clip time advances only while playing) and hands exactly that frame to OBS
 * from a cache that a worker thread fills ahead of time. The source is async and
 * unbuffered, so OBS always shows the last frame handed over: a paused clip keeps its
 * picture, and a clip that goes on air already shows the right frame on the very first
 * program frame. An empty or foreign frame cannot occur by construction.
 *
 * Off air (vMix behaviour, chosen per clip):
 *   0  pause on the last shown frame, continue from it when back on air (default);
 *   1  restart: while off air the clip waits on frame 0, on air it plays from there;
 *   2  never stop playing.
 *
 * Decoding: H.264/HEVC through the NVIDIA video engine (D3D11VA) when the stream allows
 * it, otherwise libavcodec on the CPU. Every clip keeps 8 frames ready ahead; a still clip
 * decodes them once and then idles. Short-GOP H.264 (key frame every 10 frames, no B-frames) is
 * the working codec: any frame is at most 9 decodes away.
 *
 * Sound follows the same clock: every video tick hands OBS exactly the samples the clip
 * time advanced by (nothing while still), decoded and resampled to the OBS rate on its
 * own thread from a second handle on the file, so it can never stall the picture.
 * After a jump (start, resume, seek, loop) the sound fades in over 5 ms: no click.
 */

#include <obs-module.h>
#include <util/platform.h>
#include <media-io/video-io.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <algorithm>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()

namespace {

constexpr int kAheadPlaying = 8; /* frames kept ready ahead of a playing clip */
/* A still clip keeps the same frames ready (decoded once, then the decoder idles), so it
 * starts instantly when it goes on air; about 3 MB per 1080p frame. */
constexpr int kAheadPaused = kAheadPlaying;

video_format ToObsFormat(int f)
{
	switch (f) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
		return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_YUYV422:
		return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_UYVY422:
		return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_YUV422P10LE:
		return VIDEO_FORMAT_I210;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_RGBA:
		return VIDEO_FORMAT_RGBA;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

video_colorspace ToObsSpace(const AVFrame *f)
{
	switch (f->colorspace) {
	case AVCOL_SPC_BT709:
		return f->color_trc == AVCOL_TRC_IEC61966_2_1 ? VIDEO_CS_SRGB : VIDEO_CS_709;
	case AVCOL_SPC_FCC:
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_SMPTE240M:
		return VIDEO_CS_601;
	default:
		return f->height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
	}
}

enum AVPixelFormat PickHwFormat(AVCodecContext *, const enum AVPixelFormat *fmts)
{
	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
		if (*p == AV_PIX_FMT_D3D11)
			return *p;
	/* The stream cannot go through the video engine (e.g. 4:2:2): first software format. */
	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
		const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
		if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL))
			return *p;
	}
	return fmts[0];
}

/* Decoded sound of a clip, addressed by sample number at the OBS rate (stereo float). */
class AudioTrack {
public:
	~AudioTrack() { Close(); }

	void Open(const std::string &path, uint32_t rate)
	{
		Close();
		path_ = path;
		rate_ = rate;
		stop_ = false;
		thread_ = std::thread([this]() { Run(); });
	}

	void Close()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
		}
		cv_.notify_all();
		if (thread_.joinable())
			thread_.join();
		std::lock_guard<std::mutex> lock(mutex_);
		left_.clear();
		right_.clear();
		start_ = 0;
		has_ = false;
	}

	bool Has() const { return has_.load(); }

	void SetWant(int64_t pos)
	{
		if (want_.exchange(pos) != pos)
			cv_.notify_one();
	}

	/* Samples [pos, pos + n); missing ones are silence. Returns false if any was missing. */
	bool Read(int64_t pos, int n, float *l, float *r)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const int64_t end = start_ + (int64_t)left_.size();
		bool complete = true;
		for (int i = 0; i < n; i++) {
			const int64_t p = pos + i;
			if (p >= start_ && p < end) {
				l[i] = left_[(size_t)(p - start_)];
				r[i] = right_[(size_t)(p - start_)];
			} else {
				l[i] = r[i] = 0.0f;
				complete = false;
			}
		}
		return complete;
	}

private:
	void Run()
	{
		AVFormatContext *fmt = nullptr;
		AVCodecContext *ctx = nullptr;
		SwrContext *swr = nullptr;
		AVFrame *frame = av_frame_alloc();
		AVPacket *pkt = av_packet_alloc();
		int stream = -1;
		const AVCodec *codec = nullptr;
		AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
		AVRational tb = {1, 1};
		int64_t first = 0;
		bool eof = false;

		if (avformat_open_input(&fmt, path_.c_str(), nullptr, nullptr) < 0 ||
		    avformat_find_stream_info(fmt, nullptr) < 0)
			goto done;
		stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
		if (stream < 0 || !codec)
			goto done; /* a clip without sound */
		ctx = avcodec_alloc_context3(codec);
		if (!ctx || avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar) < 0 ||
		    avcodec_open2(ctx, codec, nullptr) < 0)
			goto done;
		if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLTP, (int)rate_, &ctx->ch_layout, ctx->sample_fmt,
					ctx->sample_rate, 0, nullptr) < 0 ||
		    swr_init(swr) < 0)
			goto done;
		tb = fmt->streams[stream]->time_base;
		first = fmt->streams[stream]->start_time != AV_NOPTS_VALUE ? fmt->streams[stream]->start_time : 0;
		has_ = true;
		blog(LOG_INFO, "[MyOBS Clip] sound: %s, %d Hz, %d ch", codec->name, ctx->sample_rate,
		     ctx->ch_layout.nb_channels);

		for (;;) {
			int64_t want;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				const int64_t ahead = (int64_t)rate_; /* keep one second ready */
				cv_.wait_for(lock, std::chrono::milliseconds(100), [&]() {
					const int64_t w = want_.load();
					const int64_t end = start_ + (int64_t)left_.size();
					return stop_ || w < start_ || w > end + (int64_t)rate_ / 4 || (!eof && end < w + ahead);
				});
				if (stop_)
					break;
				want = want_.load();
				const int64_t end = start_ + (int64_t)left_.size();
				if (want < start_ || want > end + (int64_t)rate_ / 4) {
					/* Jump: start over at the wanted position. */
					left_.clear();
					right_.clear();
					start_ = want;
					lock.unlock();
					const int64_t ts = first + av_rescale_q(want, AVRational{1, (int)rate_}, tb);
					av_seek_frame(fmt, stream, ts, AVSEEK_FLAG_BACKWARD);
					avcodec_flush_buffers(ctx);
					swr_close(swr);
					swr_init(swr);
					eof = false;
					continue;
				}
				if (eof || end >= want + ahead)
					continue;
				/* Drop what lies well behind the play position. */
				const int64_t keep = want - (int64_t)rate_ / 10;
				if (keep > start_) {
					const size_t drop = (size_t)std::min<int64_t>(keep - start_, (int64_t)left_.size());
					left_.erase(left_.begin(), left_.begin() + drop);
					right_.erase(right_.begin(), right_.begin() + drop);
					start_ += (int64_t)drop;
				}
			}

			/* Decode the next audio frame. */
			int r = avcodec_receive_frame(ctx, frame);
			if (r == AVERROR(EAGAIN)) {
				if (av_read_frame(fmt, pkt) < 0) {
					avcodec_send_packet(ctx, nullptr);
					r = avcodec_receive_frame(ctx, frame);
					if (r < 0) {
						eof = true;
						continue;
					}
				} else {
					if (pkt->stream_index == stream)
						avcodec_send_packet(ctx, pkt);
					av_packet_unref(pkt);
					continue;
				}
			} else if (r < 0) {
				eof = true;
				continue;
			}

			const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp
											     : frame->pts;
			const int outMax = swr_get_out_samples(swr, frame->nb_samples);
			std::vector<float> l((size_t)std::max(outMax, 1)), rr((size_t)std::max(outMax, 1));
			uint8_t *out[2] = {reinterpret_cast<uint8_t *>(l.data()), reinterpret_cast<uint8_t *>(rr.data())};
			const int got = swr_convert(swr, out, outMax, (const uint8_t *const *)frame->extended_data,
						    frame->nb_samples);
			av_frame_unref(frame);
			if (got <= 0)
				continue;

			std::lock_guard<std::mutex> lock(mutex_);
			const int64_t end = start_ + (int64_t)left_.size();
			int64_t pos = pts != AV_NOPTS_VALUE
					      ? (int64_t)std::llround((double)(pts - first) * av_q2d(tb) * rate_)
					      : end;
			/* Small timestamp wobble: keep the sound continuous. */
			if (std::llabs(pos - end) < (int64_t)rate_ / 50)
				pos = end;
			int skip = 0;
			if (pos < end)
				skip = (int)std::min<int64_t>(end - pos, got);
			else if (pos > end && left_.empty())
				start_ = pos;
			else if (pos > end) {
				const size_t gap = (size_t)std::min<int64_t>(pos - end, (int64_t)rate_);
				left_.insert(left_.end(), gap, 0.0f);
				right_.insert(right_.end(), gap, 0.0f);
			}
			left_.insert(left_.end(), l.begin() + skip, l.begin() + got);
			right_.insert(right_.end(), rr.begin() + skip, rr.begin() + got);
		}

	done:
		av_packet_free(&pkt);
		av_frame_free(&frame);
		if (swr)
			swr_free(&swr);
		if (ctx)
			avcodec_free_context(&ctx);
		if (fmt)
			avformat_close_input(&fmt);
	}

	std::string path_;
	uint32_t rate_ = 48000;
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool stop_ = false;
	std::atomic<bool> has_{false};
	std::atomic<int64_t> want_{0};
	std::vector<float> left_, right_;
	int64_t start_ = 0; /* sample number of left_[0] */
};

class Clip {
public:
	explicit Clip(obs_source_t *source) : source_(source)
	{
		obs_source_set_async_unbuffered(source_, true);
		obs_audio_info ai = {};
		if (obs_get_audio_info(&ai) && ai.samples_per_sec > 0)
			sampleRate_ = ai.samples_per_sec;
		proc_handler_add(obs_source_get_proc_handler(source_), "void get_stats(out string text)",
				 [](void *data, calldata_t *cd) {
					 const std::string text = static_cast<Clip *>(data)->Stats();
					 calldata_set_string(cd, "text", text.c_str());
				 },
				 this);
	}

	~Clip() { Close(); }

	/* ---- settings ---- */

	void Update(obs_data_t *settings)
	{
		const std::string path = obs_data_get_string(settings, "file");
		const int offair = (int)obs_data_get_int(settings, "offair");
		const bool loop = obs_data_get_bool(settings, "loop");
		{
			std::lock_guard<std::mutex> lock(play_);
			offair_ = offair;
			loop_ = loop;
			if (offair_ == 2 && !obs_source_active(source_))
				playing_ = true;
		}
		if (path != path_) {
			Close();
			path_ = path;
			Open();
		}
	}

	/* ---- on air / off air ---- */

	void Activate()
	{
		std::lock_guard<std::mutex> lock(play_);
		/* On air a clip plays, unless the operator paused it himself or it has ended. */
		if (offair_ == 0 && !userPaused_ && !ended_)
			playing_ = true;
		if (offair_ == 1) {
			playing_ = true; /* clip time is already 0 and frame 0 is on screen */
			ended_ = false;
		}
	}

	void Deactivate()
	{
		std::lock_guard<std::mutex> lock(play_);
		if (offair_ == 0) {
			playing_ = false; /* frozen on the last shown frame */
		} else if (offair_ == 1) {
			playing_ = false;
			clipTime_ = 0.0; /* cue: the next ticks put frame 0 on screen */
		}
	}

	/* ---- every video frame (graphics thread) ---- */

	void Tick(float seconds)
	{
		const double fps = fps_.load();
		const int64_t count = count_.load();
		if (fps <= 0.0)
			return;

		int64_t idx;
		bool playing;
		double clipNow;
		{
			std::lock_guard<std::mutex> lock(play_);
			if (playing_)
				clipTime_ += seconds;
			idx = (int64_t)std::floor(clipTime_ * fps + 1e-6);
			if (count > 0 && idx >= count) {
				if (loop_) {
					idx %= count;
					clipTime_ = std::fmod(clipTime_, (double)count / fps);
				} else {
					idx = count - 1;
					clipTime_ = (double)idx / fps;
					if (playing_) {
						playing_ = false;
						ended_ = true;
					}
				}
			}
			if (idx < 0)
				idx = 0;
			playing = playing_;
			clipNow = clipTime_;
		}
		AudioTick(clipNow, playing);

		/* Both values must be updated every tick (no short-circuit): the decoder sizes its
		 * read-ahead from "playing". */
		const bool wantChanged = want_.exchange(idx) != idx;
		const bool playChanged = playingNow_.exchange(playing) != playing;
		if (wantChanged || playChanged)
			cv_.notify_one();

		/* A still clip hands its frame over again once a second and whenever it becomes
		 * visible: a frame given to OBS before the source was ready can get lost, and a
		 * still clip would otherwise never repeat it. */
		std::lock_guard<std::mutex> lock(cache_);
		bool again = false;
		if (idx == shown_ && !playing && (forceOutput_.exchange(false) || ++stillTicks_ >= 30)) {
			stillTicks_ = 0;
			again = true;
		}
		if (idx == shown_ && !again)
			return;
		auto it = frames_.find(idx);
		if (it == frames_.end()) {
			if (playing)
				late_++;
			return; /* not decoded yet: the previous frame stays on screen (never an empty one) */
		}
		Output(it->second);
		shown_ = idx;
		shownCount_++;
	}

	/* Sound for this tick: exactly the samples the clip time moved by.
	 * Never a hard edge: a stop (pause, off air) fades out over 5 ms; a jump while playing (loop,
	 * seek) fades the old place out over one frame - so the sound has no gap - and the new place
	 * fades in. */
	void AudioTick(double t, bool playing)
	{
		if (!audio_.Has())
			return;
		const int64_t target = (int64_t)std::floor(t * sampleRate_);
		audio_.SetWant(target);
		const int64_t n = target - audioPos_;
		if (!playing) {
			if (wasPlaying_)
				Emit(audioPos_, sampleRate_ / 200, -1); /* 5 ms fade-out tail */
			wasPlaying_ = false;
			audioPos_ = target;
			fadeIn_ = true;
			return;
		}
		if (n <= 0 || n > (int64_t)sampleRate_ / 5) {
			if (wasPlaying_) {
				obs_video_info ovi = {};
				const int64_t frame = obs_get_video_info(&ovi) && ovi.fps_num
							      ? (int64_t)sampleRate_ * ovi.fps_den / ovi.fps_num
							      : sampleRate_ / 50;
				Emit(audioPos_, frame, -1); /* the old place fades out, no gap */
			}
			wasPlaying_ = true;
			audioPos_ = target;
			fadeIn_ = true;
			return;
		}
		Emit(audioPos_, n, fadeIn_ ? 1 : 0);
		fadeIn_ = false;
		wasPlaying_ = true;
		audioPos_ = target;
	}

	/* n samples from pos; fade: 0 none, 1 = in over the first 5 ms, -1 = out over all n */
	void Emit(int64_t pos, int64_t n, int fade)
	{
		if (n <= 0)
			return;
		left_.resize((size_t)n);
		right_.resize((size_t)n);
		audio_.Read(pos, (int)n, left_.data(), right_.data());
		if (fade > 0) {
			const int64_t len = std::min<int64_t>(n, sampleRate_ / 200);
			for (int64_t i = 0; i < len; i++) {
				const float g = (float)(i + 1) / (float)len;
				left_[(size_t)i] *= g;
				right_[(size_t)i] *= g;
			}
		} else if (fade < 0) {
			for (int64_t i = 0; i < n; i++) {
				const float g = 1.0f - (float)(i + 1) / (float)n;
				left_[(size_t)i] *= g;
				right_[(size_t)i] *= g;
			}
		}
		obs_source_audio a = {};
		a.data[0] = reinterpret_cast<const uint8_t *>(left_.data());
		a.data[1] = reinterpret_cast<const uint8_t *>(right_.data());
		a.frames = (uint32_t)n;
		a.speakers = SPEAKERS_STEREO;
		a.format = AUDIO_FORMAT_FLOAT_PLANAR;
		a.samples_per_sec = sampleRate_;
		a.timestamp = os_gettime_ns() - (uint64_t)((double)n * 1e9 / sampleRate_);
		obs_source_output_audio(source_, &a);
	}

	/* For the switch test: "decoder, decoded, shown, late". */
	std::string Stats() const
	{
		return std::string(hwUsed_.load() ? "видеоблок" : "процессор") + ", декодировано " +
		       std::to_string(decoded_.load()) + ", показано " + std::to_string(shownCount_.load()) +
		       ", опоздало " + std::to_string(late_.load()) + (*error_.load() ? std::string(", ошибка: ") + error_.load() : std::string());
	}

	/* ---- media controls (UI) ---- */

	void PlayPause(bool pause)
	{
		std::lock_guard<std::mutex> lock(play_);
		if (!pause && ended_) {
			clipTime_ = 0.0;
			ended_ = false;
		}
		playing_ = !pause;
		userPaused_ = pause;
	}

	void Restart()
	{
		std::lock_guard<std::mutex> lock(play_);
		clipTime_ = 0.0;
		playing_ = true;
		userPaused_ = false;
		ended_ = false;
	}

	void Stop()
	{
		std::lock_guard<std::mutex> lock(play_);
		clipTime_ = 0.0;
		playing_ = false;
		ended_ = false;
	}

	int64_t TimeMs()
	{
		std::lock_guard<std::mutex> lock(play_);
		return (int64_t)(clipTime_ * 1000.0);
	}

	void SetTimeMs(int64_t ms)
	{
		std::lock_guard<std::mutex> lock(play_);
		clipTime_ = std::max(0.0, ms / 1000.0);
		ended_ = false;
	}

	int64_t DurationMs() const
	{
		const double fps = fps_.load();
		return fps > 0.0 ? (int64_t)(count_.load() * 1000.0 / fps) : 0;
	}

	obs_media_state State()
	{
		std::lock_guard<std::mutex> lock(play_);
		if (fps_.load() <= 0.0)
			return OBS_MEDIA_STATE_NONE;
		if (ended_)
			return OBS_MEDIA_STATE_ENDED;
		return playing_ ? OBS_MEDIA_STATE_PLAYING : OBS_MEDIA_STATE_PAUSED;
	}

private:
	/* ---- decoder thread ---- */

	void Open()
	{
		if (path_.empty())
			return;
		stop_ = false;
		thread_ = std::thread([this]() { Run(); });
		audio_.Open(path_, sampleRate_);
	}

	void Close()
	{
		audio_.Close();
		{
			std::lock_guard<std::mutex> lock(cache_);
			stop_ = true;
		}
		cv_.notify_all();
		if (thread_.joinable())
			thread_.join();
		std::lock_guard<std::mutex> lock(cache_);
		for (auto &kv : frames_)
			av_frame_free(&kv.second);
		frames_.clear();
		shown_ = -1;
		fps_ = 0.0;
		count_ = 0;
	}

	bool OpenFile()
	{
		if (avformat_open_input(&fmt_, path_.c_str(), nullptr, nullptr) < 0) {
			error_ = "файл не открылся";
			blog(LOG_WARNING, "[MyOBS Clip] cannot open '%s'", path_.c_str());
			return false;
		}
		if (avformat_find_stream_info(fmt_, nullptr) < 0) {
			error_ = "нет данных о потоках";
			return false;
		}
		const AVCodec *codec = nullptr;
		stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
		if (stream_ < 0 || !codec) {
			error_ = "нет видео";
			return false;
		}
		AVStream *st = fmt_->streams[stream_];
		ctx_ = avcodec_alloc_context3(codec);
		if (!ctx_ || avcodec_parameters_to_context(ctx_, st->codecpar) < 0)
			return false;

		hw_ = false;
		if (st->codecpar->codec_id == AV_CODEC_ID_H264 || st->codecpar->codec_id == AV_CODEC_ID_HEVC) {
			if (av_hwdevice_ctx_create(&hwDevice_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) == 0) {
				ctx_->hw_device_ctx = av_buffer_ref(hwDevice_);
				ctx_->get_format = PickHwFormat;
				hw_ = true;
			}
		}
		if (!hw_) {
			ctx_->thread_count = 0;
			ctx_->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
		}
		if (avcodec_open2(ctx_, codec, nullptr) < 0) {
			error_ = "декодер не открылся";
			return false;
		}
		hwUsed_ = hw_;

		const AVRational rate = av_guess_frame_rate(fmt_, st, nullptr);
		if (rate.num <= 0 || rate.den <= 0)
			return false;
		rate_ = rate;
		timeBase_ = st->time_base;
		start_ = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
		int64_t count = st->nb_frames;
		if (count <= 0 && fmt_->duration > 0)
			count = (int64_t)std::llround(fmt_->duration / (double)AV_TIME_BASE * av_q2d(rate));
		count_ = count;
		fps_ = av_q2d(rate);
		blog(LOG_INFO, "[MyOBS Clip] '%s': %s, %dx%d, %.3f fps, %lld frames, %s decode", path_.c_str(),
		     codec->name, ctx_->width, ctx_->height, av_q2d(rate), (long long)count,
		     hw_ ? "video engine (D3D11VA)" : "CPU");
		return true;
	}

	void CloseFile()
	{
		if (sws_)
			sws_freeContext(sws_);
		sws_ = nullptr;
		if (ctx_)
			avcodec_free_context(&ctx_);
		if (hwDevice_)
			av_buffer_unref(&hwDevice_);
		if (fmt_)
			avformat_close_input(&fmt_);
	}

	int64_t IndexOf(const AVFrame *f) const
	{
		int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
		if (pts == AV_NOPTS_VALUE)
			return -1;
		return (int64_t)std::llround((double)(pts - start_) * av_q2d(timeBase_) * av_q2d(rate_));
	}

	void Seek(int64_t idx)
	{
		const int64_t ts = start_ + av_rescale_q(idx, av_inv_q(rate_), timeBase_);
		av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(ctx_);
		eof_ = false;
		draining_ = false;
	}

	/* Next decoded frame in system memory, or nullptr at the end of the file / on error. */
	AVFrame *DecodeNext()
	{
		AVFrame *frame = av_frame_alloc();
		AVPacket *pkt = av_packet_alloc();
		AVFrame *result = nullptr;
		for (;;) {
			const int r = avcodec_receive_frame(ctx_, frame);
			if (r == 0) {
				result = ToSystemFrame(frame);
				break;
			}
			if (r == AVERROR_EOF) {
				eof_ = true;
				break;
			}
			if (r != AVERROR(EAGAIN))
				break;
			if (draining_) {
				eof_ = true;
				break;
			}
			const int rr = av_read_frame(fmt_, pkt);
			if (rr < 0) {
				avcodec_send_packet(ctx_, nullptr);
				draining_ = true;
				continue;
			}
			if (pkt->stream_index == stream_)
				avcodec_send_packet(ctx_, pkt);
			av_packet_unref(pkt);
		}
		av_packet_free(&pkt);
		if (result != frame)
			av_frame_free(&frame);
		return result;
	}

	/* Video engine frames are copied to memory; formats OBS cannot show are converted to I420. */
	AVFrame *ToSystemFrame(AVFrame *frame)
	{
		AVFrame *f = frame;
		if (frame->format == AV_PIX_FMT_D3D11) {
			AVFrame *sw = av_frame_alloc();
			if (av_hwframe_transfer_data(sw, frame, 0) < 0) {
				av_frame_free(&sw);
				error_ = "копирование кадра из видеоблока";
				return nullptr;
			}
			av_frame_copy_props(sw, frame);
			f = sw;
		}
		if (ToObsFormat(f->format) == VIDEO_FORMAT_NONE) {
			AVFrame *conv = av_frame_alloc();
			conv->format = AV_PIX_FMT_YUV420P;
			conv->width = f->width;
			conv->height = f->height;
			if (av_frame_get_buffer(conv, 32) < 0) {
				av_frame_free(&conv);
				if (f != frame)
					av_frame_free(&f);
				return nullptr;
			}
			sws_ = sws_getCachedContext(sws_, f->width, f->height, (AVPixelFormat)f->format, f->width,
						    f->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
			if (sws_)
				sws_scale(sws_, f->data, f->linesize, 0, f->height, conv->data, conv->linesize);
			av_frame_copy_props(conv, f);
			if (f != frame)
				av_frame_free(&f);
			f = conv;
		}
		return f;
	}

	/* First frame of the wanted window that is not decoded yet. Called under cache_. */
	bool NextMissing(int64_t &target)
	{
		const int64_t want = want_.load();
		const int64_t count = count_.load();
		const int ahead = playingNow_.load() ? kAheadPlaying : kAheadPaused;
		for (int k = 0; k < ahead; k++) {
			int64_t idx = want + k;
			if (count > 0 && idx >= count) {
				if (!loop_)
					break;
				idx %= count;
			}
			if (!frames_.count(idx) && !failed_.count(idx)) {
				target = idx;
				return true;
			}
		}
		return false;
	}

	bool InWindow(int64_t idx)
	{
		const int64_t want = want_.load();
		const int64_t count = count_.load();
		const int ahead = playingNow_.load() ? kAheadPlaying : kAheadPaused;
		for (int k = 0; k < ahead; k++) {
			int64_t w = want + k;
			if (count > 0 && w >= count)
				w %= count;
			if (w == idx)
				return true;
		}
		/* Keep the frame on screen until another one replaces it. */
		return idx == shown_;
	}

	void Evict()
	{
		for (auto it = frames_.begin(); it != frames_.end();) {
			if (!InWindow(it->first)) {
				av_frame_free(&it->second);
				it = frames_.erase(it);
			} else {
				++it;
			}
		}
		if (failed_.size() > 64)
			failed_.clear();
	}

	void Run()
	{
		if (!OpenFile()) {
			CloseFile();
			return;
		}
		int64_t next = -1; /* index the decoder will produce next without a seek */
		for (;;) {
			int64_t target = 0;
			{
				std::unique_lock<std::mutex> lock(cache_);
				Evict();
				cv_.wait_for(lock, std::chrono::milliseconds(100),
					     [&]() { return stop_ || NextMissing(target); });
				if (stop_)
					break;
				if (!NextMissing(target))
					continue;
			}

			/* Sequential decoding unless the target is behind or far ahead (one GOP or more). */
			if (next < 0 || target < next || target > next + 12) {
				Seek(target);
				next = -1;
			}

			AVFrame *f = DecodeNext();
			if (!f) {
				if (eof_) {
					/* End of file: frames past it do not exist; looping wraps via the window. */
					std::lock_guard<std::mutex> lock(cache_);
					if (lastIndex_ >= 0 && (count_.load() <= 0 || lastIndex_ + 1 < count_.load()))
						count_ = lastIndex_ + 1; /* the container promised more frames than it has */
					if (count_.load() <= 0 || target >= count_.load())
						failed_.insert(target);
				}
				next = -1;
				continue;
			}
			const int64_t idx = IndexOf(f);
			if (idx < 0) {
				av_frame_free(&f);
				continue;
			}
			next = idx + 1;
			lastIndex_ = std::max(lastIndex_, idx);
			decoded_++;
			std::lock_guard<std::mutex> lock(cache_);
			if (!frames_.count(idx) && InWindow(idx)) {
				frames_[idx] = f;
				failed_.erase(idx);
			} else {
				av_frame_free(&f);
			}
		}
		CloseFile();
	}

	/* ---- hand a frame to OBS (graphics thread, under cache_) ---- */

	void Output(const AVFrame *f)
	{
		obs_source_frame out = {};
		const video_format format = ToObsFormat(f->format);
		if (format == VIDEO_FORMAT_NONE)
			return;
		for (int i = 0; i < MAX_AV_PLANES && i < AV_NUM_DATA_POINTERS; i++) {
			out.data[i] = f->data[i];
			out.linesize[i] = (uint32_t)std::abs(f->linesize[i]);
		}
		out.width = (uint32_t)f->width;
		out.height = (uint32_t)f->height;
		out.format = format;
		out.timestamp = os_gettime_ns();
		const bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P ||
				  f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P;
		const video_range_type range = full ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
		out.full_range = full;
		out.trc = VIDEO_TRC_DEFAULT;
		if (!video_format_get_parameters_for_format(ToObsSpace(f), range, format, out.color_matrix,
							    out.color_range_min, out.color_range_max))
			return;
		obs_source_output_video(source_, &out);
	}

	obs_source_t *source_;
	std::string path_;

	/* sound (graphics thread, except the track's own thread) */
	AudioTrack audio_;
	uint32_t sampleRate_ = 48000;
	int64_t audioPos_ = 0;
	bool fadeIn_ = true;
	bool wasPlaying_ = false;
	std::vector<float> left_, right_;

	/* playback (play_) */
	std::mutex play_;
	double clipTime_ = 0.0;
	bool playing_ = false;
	bool userPaused_ = false;
	bool ended_ = false;
	int offair_ = 0;
	std::atomic<bool> loop_{false};

	/* shared with the decoder (cache_) */
	std::mutex cache_;
	std::condition_variable cv_;
	std::map<int64_t, AVFrame *> frames_;
	std::set<int64_t> failed_; /* frames that do not exist (past the real end of the file) */
	bool stop_ = false;
	std::thread thread_;
	std::atomic<int64_t> want_{0};
	std::atomic<bool> playingNow_{false};
	std::atomic<double> fps_{0.0};
	std::atomic<int64_t> count_{0};
	int64_t shown_ = -1;
	int stillTicks_ = 0;
	std::atomic<bool> forceOutput_{false};

public:
	void Show() { forceOutput_ = true; }

private:
	std::atomic<uint64_t> late_{0};
	std::atomic<uint64_t> shownCount_{0};
	std::atomic<uint64_t> decoded_{0};
	std::atomic<bool> hwUsed_{false};
	std::atomic<const char *> error_{""};

	/* decoder thread only */
	AVFormatContext *fmt_ = nullptr;
	AVCodecContext *ctx_ = nullptr;
	AVBufferRef *hwDevice_ = nullptr;
	SwsContext *sws_ = nullptr;
	int stream_ = -1;
	bool hw_ = false;
	bool eof_ = false;
	bool draining_ = false;
	AVRational rate_ = {0, 1};
	AVRational timeBase_ = {1, 1};
	int64_t start_ = 0;
	int64_t lastIndex_ = -1;
};

/* ---------------------------------------------------------------------------------------- */

const char *GetName(void *)
{
	return "Клип";
}

void *Create(obs_data_t *settings, obs_source_t *source)
{
	auto *c = new Clip(source);
	c->Update(settings);
	return c;
}

void Destroy(void *data)
{
	delete static_cast<Clip *>(data);
}

void Defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "offair", 0);
	obs_data_set_default_bool(settings, "loop", false);
}

obs_properties_t *Properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_path(props, "file", "Файл", OBS_PATH_FILE,
				"Видео (*.mp4 *.mov *.mkv *.avi *.mxf *.m4v *.ts *.mts *.webm);;Все файлы (*.*)", nullptr);
	obs_property_t *p = obs_properties_add_list(props, "offair", "Вне эфира", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Пауза, продолжить с того же кадра", 0);
	obs_property_list_add_int(p, "С начала при выходе в эфир", 1);
	obs_property_list_add_int(p, "Не останавливать проигрывание", 2);
	obs_properties_add_bool(props, "loop", "Повтор");
	return props;
}

} // namespace

bool obs_module_load(void)
{
	obs_source_info info = {};
	info.id = "myobs_clip";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			    OBS_SOURCE_CONTROLLABLE_MEDIA;
	info.get_name = GetName;
	info.create = Create;
	info.destroy = Destroy;
	info.get_defaults = Defaults;
	info.get_properties = Properties;
	info.update = [](void *data, obs_data_t *settings) { static_cast<Clip *>(data)->Update(settings); };
	info.activate = [](void *data) { static_cast<Clip *>(data)->Activate(); };
	info.show = [](void *data) { static_cast<Clip *>(data)->Show(); };
	info.deactivate = [](void *data) { static_cast<Clip *>(data)->Deactivate(); };
	info.video_tick = [](void *data, float seconds) { static_cast<Clip *>(data)->Tick(seconds); };
	info.media_play_pause = [](void *data, bool pause) { static_cast<Clip *>(data)->PlayPause(pause); };
	info.media_restart = [](void *data) { static_cast<Clip *>(data)->Restart(); };
	info.media_stop = [](void *data) { static_cast<Clip *>(data)->Stop(); };
	info.media_get_duration = [](void *data) { return static_cast<Clip *>(data)->DurationMs(); };
	info.media_get_time = [](void *data) { return static_cast<Clip *>(data)->TimeMs(); };
	info.media_set_time = [](void *data, int64_t ms) { static_cast<Clip *>(data)->SetTimeMs(ms); };
	info.media_get_state = [](void *data) { return static_cast<Clip *>(data)->State(); };
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	obs_register_source(&info);
	blog(LOG_INFO, "[MyOBS Clip] loaded");
	return true;
}

const char *obs_module_name(void)
{
	return "MyOBS Clip";
}
