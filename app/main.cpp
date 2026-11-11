/* MyOBS — live production program on the OBS engine (libobs).
 *
 * Stage 1 skeleton: starts the engine on the discrete GPU, loads the engine plugins and our
 * own ones («Клип»), shows the Preview and Program monitors at the project format and the
 * load line. The switcher, inputs and the rest of the interface are built on this next.
 *
 * The log is written to %LOCALAPPDATA%/MyOBS/logs (one file per start).
 */

#include <obs.hpp>
#include <util/base.h>
#include <util/platform.h>

#include "plugins.hpp"

#include <QApplication>
#include <QDateTime>
#include <QFrame>
#include <QStyle>
#include <QTime>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include "display.hpp"
#ifdef _WIN32
#include "gpu.hpp"
#endif


namespace {

/* ---- log to file ---- */

FILE *g_log = nullptr;
std::mutex g_logMutex;

void LogHandler(int level, const char *format, va_list args, void *)
{
	char buf[8192];
	std::vsnprintf(buf, sizeof(buf), format, args);
	std::lock_guard<std::mutex> lock(g_logMutex);
	if (g_log) {
		const char *tag = level <= LOG_ERROR ? "error" : level <= LOG_WARNING ? "warning" : "info";
		std::fprintf(g_log, "%s %s: %s\n", QTime::currentTime().toString("HH:mm:ss.zzz").toUtf8().constData(),
			     tag, buf);
		std::fflush(g_log);
	}
}

void OpenLog()
{
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/MyOBS/logs";
	QDir().mkpath(dir);
	const QString path = dir + "/myobs-" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") + ".txt";
	g_log = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"w");
	base_set_log_handler(LogHandler, nullptr);
}

const char *kStyle = R"(
QWidget#MyOBS { background: #15171a; color: #d8dade; font-size: 12px; }
QLabel#monitorTitle { color: #c9ccd1; font-size: 12px; padding: 1px 0; }
QLabel#load { color: #8a8f97; font-family: Consolas, monospace; font-size: 11px; }
QLabel#loadBad { color: #ff6b5a; font-family: Consolas, monospace; font-size: 11px; }
)";

/* ---- main window (skeleton) ---- */

class MainWindow : public QWidget {
public:
	explicit MainWindow(const std::string &gpu) : gpu_(QString::fromUtf8(gpu.c_str()))
	{
		setObjectName("MyOBS");
		setAttribute(Qt::WA_StyledBackground);
		setStyleSheet(kStyle);
		setWindowTitle("MyOBS");
		resize(1440, 900);

		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(8, 8, 8, 6);
		root->setSpacing(6);

		preview_ = new SourceDisplay(SourceDisplay::Mode::Source, this);
		program_ = new SourceDisplay(SourceDisplay::Mode::Program, this);
		auto *top = new QHBoxLayout();
		top->setSpacing(8);
		top->addWidget(Monitor(previewTitle_, preview_, "#f28c1e"), 1);
		top->addWidget(Monitor(programTitle_, program_, "#e0312b"), 1);
		previewTitle_->setText("PREVIEW");
		programTitle_->setText("PROGRAM");
		root->addLayout(top, 5);
		root->addStretch(4);

		load_ = new QLabel(this);
		load_->setObjectName("load");
		root->addWidget(load_);

		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this]() { Tick(); });
		timer->start(500);
	}

	~MainWindow() override
	{
		preview_->DestroyDisplay();
		program_->DestroyDisplay();
	}

private:
	QWidget *Monitor(QLabel *&label, SourceDisplay *display, const char *color)
	{
		label = new QLabel(this);
		label->setObjectName("monitorTitle");
		auto *frame = new QFrame(this);
		frame->setObjectName("monitorFrame");
		frame->setStyleSheet(QString("QFrame#monitorFrame { border: 2px solid %1; background:#0b0c0e; }").arg(color));
		auto *fl = new QVBoxLayout(frame);
		fl->setContentsMargins(2, 2, 2, 2);
		fl->addWidget(display);
		auto *holder = new AspectHolder(label, frame, this);
		holders_.push_back(holder);
		return holder;
	}

	void Tick()
	{
		const bool visible = isVisible() && !isMinimized();
		obs_video_info ovi = {};
		if (obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height)
			for (AspectHolder *h : holders_)
				h->SetAspect((double)ovi.base_width / ovi.base_height);
		preview_->Sync(visible);
		program_->Sync(visible);

		const double budget = ovi.fps_num ? 1000.0 * ovi.fps_den / ovi.fps_num : 0.0;
		const double renderMs = obs_get_average_frame_time_ns() / 1000000.0;
		const uint32_t total = obs_get_total_frames();
		const uint32_t lagged = obs_get_lagged_frames();
		video_t *video = obs_get_video();
		const uint32_t encSkipped = video ? video_output_get_skipped_frames(video) : 0;
		const bool bad = (budget > 0 && renderMs > budget * 0.8) || lagged > lastLagged_ || encSkipped > lastSkipped_;
		lastLagged_ = lagged;
		lastSkipped_ = encSkipped;
		load_->setObjectName(bad ? "loadBad" : "load");
		load_->style()->unpolish(load_);
		load_->style()->polish(load_);
		load_->setText(QString("GPU %1   |   %2x%3 %4 к/с   |   рендер %5 / %6 мс   |   пропуск рендер %7 из %8   |   пропуск кодер %9")
				       .arg(gpu_)
				       .arg(ovi.base_width)
				       .arg(ovi.base_height)
				       .arg(ovi.fps_den ? ovi.fps_num / ovi.fps_den : 0)
				       .arg(renderMs, 0, 'f', 1)
				       .arg(budget, 0, 'f', 1)
				       .arg(lagged)
				       .arg(total)
				       .arg(encSkipped));
	}

	QString gpu_;
	SourceDisplay *preview_ = nullptr;
	SourceDisplay *program_ = nullptr;
	QLabel *previewTitle_ = nullptr;
	QLabel *programTitle_ = nullptr;
	QLabel *load_ = nullptr;
	std::vector<AspectHolder *> holders_;
	uint32_t lastLagged_ = 0;
	uint32_t lastSkipped_ = 0;
};

} // namespace

int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	QApplication::setApplicationName("MyOBS");
	QApplication::setOrganizationName("MyOBS");
	OpenLog();

	if (!obs_startup("ru-RU", nullptr, nullptr)) {
		QMessageBox::critical(nullptr, "MyOBS", "Движок не запустился.");
		return 1;
	}

	QSettings settings;
	std::string gpu = "default";
	uint32_t adapter = 0;
#ifdef _WIN32
	adapter = MyOBSGpu::ResolveRenderAdapter(settings.value("video/renderGpu").toString().toStdString(), gpu);
	settings.setValue("video/renderGpu", QString::fromStdString(gpu));
#endif


	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = (uint32_t)settings.value("video/fps", 50).toUInt();
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = (uint32_t)settings.value("video/width", 1920).toUInt();
	ovi.base_height = ovi.output_height = (uint32_t)settings.value("video/height", 1080).toUInt();
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.adapter = adapter;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	const int video = obs_reset_video(&ovi);
	if (video != OBS_VIDEO_SUCCESS) {
		QMessageBox::critical(nullptr, "MyOBS", QString("Видео не запустилось (код %1).").arg(video));
		obs_shutdown();
		return 1;
	}
	obs_audio_info ai = {};
	ai.samples_per_sec = 48000;
	ai.speakers = SPEAKERS_STEREO;
	obs_reset_audio(&ai);

	myobs::LoadPlugins();
	obs_log_loaded_modules();

	int result = 0;
	{
		auto window = std::make_unique<MainWindow>(gpu);
		window->show();
		result = app.exec();
	} /* displays go before the engine */

	obs_shutdown();
	if (g_log)
		std::fclose(g_log);
	return result;
}
