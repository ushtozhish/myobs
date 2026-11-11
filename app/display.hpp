/* MyOBS: native libobs display inside a Qt widget (Windows), plus a holder that keeps a
 * monitor at the project format. Program mode draws the real output texture (transitions
 * included); Source mode draws one source. */
#pragma once

#include <obs.hpp>

#include <QLabel>
#include <QPaintEngine>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

class SourceDisplay : public QWidget {
public:
	enum class Mode { Program, Source };

	explicit SourceDisplay(Mode mode, QWidget *parent = nullptr) : QWidget(parent), mode_(mode)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMinimumSize(16, 16);
	}

	~SourceDisplay() override { DestroyDisplay(); }

	QPaintEngine *paintEngine() const override { return nullptr; }

	void SetSource(obs_source_t *source)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		weak_ = OBSGetWeakRef(source);
	}

	/* Extra GPU work done once per frame from this display (used for tile thumbnails). */
	void (*onFrame)() = nullptr;

	void DestroyDisplay()
	{
		if (display_) {
			obs_display_remove_draw_callback(display_, Draw, this);
			obs_display_destroy(display_);
			display_ = nullptr;
			hwnd_ = 0;
		}
	}

	/* Called by the window twice a second and on show/resize: keeps the OBS display bound to
	 * the current native window. A display left on an old window handle, on another screen or
	 * switched off is exactly what shows up as a white monitor. */
	void Sync(bool visible)
	{
		if (!visible) {
			if (display_)
				obs_display_set_enabled(display_, false);
			return;
		}
		const quintptr hwnd = (quintptr)winId();
		QScreen *screen = window() ? window()->screen() : nullptr;
		if (display_ && (hwnd != hwnd_ || screen != screen_)) {
			blog(LOG_INFO, "[MyOBS Switcher] monitor window changed, recreating display");
			DestroyDisplay();
		}
		if (!display_)
			CreateDisplay();
		if (display_) {
			obs_display_set_enabled(display_, true);
			QSize s = PixelSize();
			if (s != size_) {
				obs_display_resize(display_, (uint32_t)s.width(), (uint32_t)s.height());
				size_ = s;
			}
		}
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		QTimer::singleShot(0, this, [this]() { Sync(isVisible()); });
	}

	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		Sync(isVisible());
	}

private:
	QSize PixelSize() const
	{
		const qreal r = devicePixelRatioF();
		return QSize(std::max(1, (int)(width() * r)), std::max(1, (int)(height() * r)));
	}

	void CreateDisplay()
	{
		if (display_ || !isVisible())
			return;
		QWindow *w = windowHandle();
		if (!w || !w->isExposed())
			return;
		QSize s = PixelSize();
		gs_init_data info = {};
		info.cx = (uint32_t)s.width();
		info.cy = (uint32_t)s.height();
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
		hwnd_ = (quintptr)winId();
#ifdef _WIN32
		info.window.hwnd = (HWND)hwnd_;
#endif
		display_ = obs_display_create(&info, 0xFF0B0C0E);
		if (display_) {
			size_ = s;
			screen_ = window() ? window()->screen() : nullptr;
			obs_display_add_draw_callback(display_, Draw, this);
		} else {
			hwnd_ = 0;
			blog(LOG_WARNING, "[MyOBS Switcher] obs_display_create failed");
		}
	}

	/* Graphics thread. */
	static void Draw(void *param, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<SourceDisplay *>(param);
		if (self->onFrame)
			self->onFrame();
		obs_video_info ovi = {};
		if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0 || cx == 0 || cy == 0)
			return;

		const double sx = (double)cx / ovi.base_width;
		const double sy = (double)cy / ovi.base_height;
		const double scale = std::min(sx, sy);
		const int w = (int)(ovi.base_width * scale);
		const int h = (int)(ovi.base_height * scale);
		const int x = ((int)cx - w) / 2;
		const int y = ((int)cy - h) / 2;

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
		gs_set_viewport(x, y, w, h);

		if (self->mode_ == Mode::Program) {
			obs_render_main_texture();
		} else {
			OBSSource source;
			{
				std::lock_guard<std::mutex> lock(self->mutex_);
				source = OBSGetStrongRef(self->weak_);
			}
			if (source)
				obs_source_video_render(source);
		}

		gs_projection_pop();
		gs_viewport_pop();
	}

	Mode mode_;
	obs_display_t *display_ = nullptr;
	quintptr hwnd_ = 0;
	QScreen *screen_ = nullptr;
	QSize size_;
	std::mutex mutex_;
	OBSWeakSource weak_;
};

/* ---------------------------------------------------------------------------------------- */
/* Monitor holder: title on top and the framed display below, the frame always kept at the  */
/* project format (16:9 or 9:16) and centred in whatever space the window gives it.          */

class AspectHolder : public QWidget {
public:
	AspectHolder(QLabel *title, QWidget *frame, QWidget *parent) : QWidget(parent), title_(title), frame_(frame)
	{
		title_->setParent(this);
		frame_->setParent(this);
		setMinimumSize(240, 160);
	}

	void SetAspect(double aspect)
	{
		if (aspect > 0.0 && std::abs(aspect - aspect_) > 1e-6) {
			aspect_ = aspect;
			Place();
		}
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		Place();
	}

private:
	void Place()
	{
		const int titleH = title_->sizeHint().height() + 2;
		const int availW = width();
		const int availH = std::max(1, height() - titleH);
		int w = availW;
		int h = (int)(w / aspect_);
		if (h > availH) {
			h = availH;
			w = (int)(h * aspect_);
		}
		const int x = (availW - w) / 2;
		const int y = titleH + (availH - h) / 2;
		title_->setGeometry(x, y - titleH, std::max(w, 1), titleH);
		frame_->setGeometry(x, y, w, h);
	}

	QLabel *title_;
	QWidget *frame_;
	double aspect_ = 16.0 / 9.0;
};

