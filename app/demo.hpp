/* Demo screenshots: `myobs.exe --demo-screenshots <folder>`.
 *
 * Builds a demo set of inputs (generated pictures), runs a small switching scenario and saves
 * screenshots of the main window. Monitors and tile thumbnails are captured from the engine
 * itself (rendered into a texture and read back), so the screenshots work on a machine without a
 * visible desktop as well (CI). Settings of the demo are kept apart from the user's settings. */
#pragma once

#include <obs.hpp>
#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QSettings>
#include <QWidget>

#include <utility>
#include <vector>

namespace demo {

/* Renders a source (or the program output when source is null) into a w x h image. */
inline QImage Capture(obs_source_t *source, uint32_t w, uint32_t h)
{
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	QImage out;
	obs_enter_graphics();
	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
	if (tr && stage && gs_texrender_begin(tr, w, h)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		if (source)
			obs_source_video_render(source);
		else
			obs_render_main_texture();
		gs_blend_state_pop();
		gs_texrender_end(tr);
		gs_stage_texture(stage, gs_texrender_get_texture(tr));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			out = QImage(data, (int)w, (int)h, (int)linesize, QImage::Format_RGBA8888).copy();
			gs_stagesurface_unmap(stage);
		}
	}
	if (stage)
		gs_stagesurface_destroy(stage);
	if (tr)
		gs_texrender_destroy(tr);
	obs_leave_graphics();
	return out;
}

/* Window screenshot with the engine pictures painted into the display widgets. */
inline QImage Shot(QWidget *window, const std::vector<std::pair<QWidget *, obs_source_t *>> &displays, bool withProgram,
		   QWidget *programWidget)
{
	QImage img = window->grab().toImage().convertToFormat(QImage::Format_RGB32);
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	QPainter g(&img);
	g.setRenderHint(QPainter::SmoothPixmapTransform);
	auto paint = [&](QWidget *w, obs_source_t *src, bool program) {
		const QRect r(w->mapTo(window, QPoint(0, 0)), w->size());
		const double scale = std::min((double)r.width() / ovi.base_width, (double)r.height() / ovi.base_height);
		const int pw = std::max(1, (int)(ovi.base_width * scale)), ph = std::max(1, (int)(ovi.base_height * scale));
		g.fillRect(r, QColor("#0b0c0e"));
		const QImage pic = Capture(program ? nullptr : src, (uint32_t)pw, (uint32_t)ph);
		if (!pic.isNull())
			g.drawImage(QPoint(r.x() + (r.width() - pw) / 2, r.y() + (r.height() - ph) / 2), pic);
	};
	for (auto &[w, src] : displays)
		paint(w, src, false);
	if (withProgram && programWidget)
		paint(programWidget, nullptr, true);
	return img;
}

/* Demo pictures and the demo input list (written into the demo settings before the window opens). */
inline void Prepare(const QString &dir)
{
	QDir().mkpath(dir);
	struct Pic {
		const char *name, *title, *tab;
		QColor a, b;
	};
	const Pic pics[] = {
		{"Камера 1", "КАМЕРА 1 · ВЕДУЩИЙ", "Камеры", QColor("#1d3b5c"), QColor("#0d1a29")},
		{"Камера 2", "КАМЕРА 2 · ГОСТЬ", "Камеры", QColor("#5c3a1d"), QColor("#29190d")},
		{"Камера 3", "КАМЕРА 3 · ОБЩИЙ", "Камеры", QColor("#2d5c3a"), QColor("#0f2914")},
		{"Заставка", "ЗАСТАВКА ЭФИРА", "Графика", QColor("#4a1d5c"), QColor("#1e0d29")},
		{"Титр", "ТИТР · ИМЯ ФАМИЛИЯ", "Графика", QColor("#5c1d24"), QColor("#290d10")},
	};
	QJsonArray inputs;
	for (const Pic &p : pics) {
		QImage img(1920, 1080, QImage::Format_RGB32);
		QPainter g(&img);
		QLinearGradient grad(0, 0, 1920, 1080);
		grad.setColorAt(0, p.a);
		grad.setColorAt(1, p.b);
		g.fillRect(img.rect(), grad);
		g.setPen(QPen(QColor(255, 255, 255, 40), 2));
		for (int x = 0; x < 1920; x += 160)
			g.drawLine(x, 0, x, 1080);
		for (int y = 0; y < 1080; y += 160)
			g.drawLine(0, y, 1920, y);
		QFont f("Segoe UI", 96, QFont::Bold);
		g.setFont(f);
		g.setPen(Qt::white);
		g.drawText(img.rect(), Qt::AlignCenter, QString::fromUtf8(p.title));
		g.end();
		const QString file = dir + "/" + QString::fromUtf8(p.name) + ".png";
		img.save(file);
		QJsonObject o;
		o["type"] = "image";
		o["name"] = QString::fromUtf8(p.name);
		o["tab"] = QString::fromUtf8(p.tab);
		o["volume"] = 1.0;
		o["muted"] = false;
		o["settings"] = QJsonObject{{"file", file}};
		inputs.append(o);
	}
	QJsonObject color;
	color["type"] = "color";
	color["name"] = "Чёрный";
	color["tab"] = "";
	color["volume"] = 1.0;
	color["muted"] = false;
	color["settings"] = QJsonObject{{"color", (qint64)0xFF000000LL}, {"width", 1920}, {"height", 1080}};
	inputs.append(color);
	QSettings s;
	s.clear();
	s.setValue("inputs/list", QJsonDocument(inputs).toJson(QJsonDocument::Compact));
	QJsonArray tabs;
	tabs.append(QJsonObject{{"name", "Камеры"}, {"color", "#3a8ee6"}});
	tabs.append(QJsonObject{{"name", "Графика"}, {"color", "#b56ce0"}});
	s.setValue("inputs/tabs", QJsonDocument(tabs).toJson(QJsonDocument::Compact));
	s.setValue("inputs/currentTab", "");
	s.setValue("switcher/fadeMs", 2000);
	s.setValue("video/fps", 30);
}

} // namespace demo
