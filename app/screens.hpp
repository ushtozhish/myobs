/* Screen outputs: any input, Program or Preview full screen on any monitor (LED wall, hall screens,
 * presenter monitor), independently of what is on air. An input on a screen runs (clips play) while
 * off air - see core/screenfeeds.hpp. The choice per monitor is saved; the trigger action
 * «Экран: показать» switches a screen to an input. */
#pragma once

#include <obs.hpp>

#include "display.hpp"
#include "screenfeeds.hpp"
#include "switcher.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <functional>
#include <map>
#include <memory>

class ScreenWindow : public QWidget {
public:
	ScreenWindow(SourceDisplay::Mode mode) : QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool)
	{
		setAttribute(Qt::WA_ShowWithoutActivating);
		setStyleSheet("background: black;");
		auto *l = new QVBoxLayout(this);
		l->setContentsMargins(0, 0, 0, 0);
		display_ = new SourceDisplay(mode, this);
		l->addWidget(display_);
	}
	void PlaceOn(QScreen *screen)
	{
		setGeometry(screen->geometry());
		winId();
		if (windowHandle())
			windowHandle()->setScreen(screen);
		showFullScreen();
	}
	SourceDisplay *Display() const { return display_; }

private:
	SourceDisplay *display_;
};

class ScreenOutputs {
public:
	using Kind = myobs::ScreenFeeds::Kind;

	/* names(): current input names, for the dialog */
	ScreenOutputs(myobs::Switcher *sw, std::function<QStringList()> names) : sw_(sw), names_(std::move(names))
	{
		timer_ = new QTimer();
		QObject::connect(timer_, &QTimer::timeout, [this]() { Refresh(); });
		timer_->start(250);
	}

	~ScreenOutputs()
	{
		delete timer_;
		windows_.clear();
	}

	/* index: input for Kind::Input */
	void Set(const QString &screenId, Kind kind, int index = -1, bool save = true)
	{
		QScreen *screen = Find(screenId);
		if (kind == Kind::Input && !sw_->InputScene(index))
			kind = Kind::Off;
		feeds_.Show(screenId.toStdString(), kind, kind == Kind::Input ? sw_->InputScene(index) : nullptr);
		chosen_[screenId] = {kind, index};
		windows_.erase(screenId);
		if (kind != Kind::Off && screen) {
			auto w = std::make_unique<ScreenWindow>(kind == Kind::Program ? SourceDisplay::Mode::Program
										: SourceDisplay::Mode::Source);
			if (kind == Kind::Input)
				w->Display()->SetSource(sw_->InputScene(index));
			w->PlaceOn(screen);
			windows_[screenId] = std::move(w);
		}
		if (save)
			QSettings().setValue("screens/" + screenId, Encode(kind, index));
	}

	void Restore()
	{
		for (QScreen *sc : QGuiApplication::screens()) {
			const QString v = QSettings().value("screens/" + sc->name()).toString();
			int index = -1;
			const Kind kind = Decode(v, index);
			if (kind != Kind::Off)
				Set(sc->name(), kind, index, false);
		}
	}

	/* an input was removed: screens showing it go off, later indices move down */
	void InputRemoved(int index)
	{
		std::map<QString, std::pair<Kind, int>> chosen = chosen_;
		for (auto &[id, c] : chosen)
			if (c.first == Kind::Input && c.second >= index)
				Set(id, c.second == index ? Kind::Off : Kind::Input, c.second == index ? -1 : c.second - 1);
	}

	void Dialog(QWidget *parent)
	{
		QDialog d(parent);
		d.setWindowTitle("Экраны — вывод во весь экран");
		auto *root = new QVBoxLayout(&d);
		auto *grid = new QGridLayout();
		root->addLayout(grid);
		const QStringList names = names_();
		const auto screens = QGuiApplication::screens();
		QScreen *mine = parent && parent->window() ? parent->window()->screen() : nullptr;
		for (int i = 0; i < screens.size(); i++) {
			QScreen *sc = screens[i];
			QString title = QString("Экран %1: %2 (%3×%4)")
						.arg(i + 1)
						.arg(sc->model().isEmpty() ? sc->name() : sc->model())
						.arg((int)(sc->size().width() * sc->devicePixelRatio()))
						.arg((int)(sc->size().height() * sc->devicePixelRatio()));
			if (sc == mine)
				title += "  — здесь окно MyOBS";
			grid->addWidget(new QLabel(title, &d), i, 0);
			auto *combo = new QComboBox(&d);
			combo->addItem("Выкл", "off");
			combo->addItem("Program (эфир)", "program");
			combo->addItem("Preview", "preview");
			for (int k = 0; k < names.size(); k++)
				combo->addItem(names[k], QString("input:%1").arg(k));
			auto it = chosen_.find(sc->name());
			if (it != chosen_.end())
				combo->setCurrentIndex(std::max(0, combo->findData(Encode(it->second.first, it->second.second))));
			const QString id = sc->name();
			QObject::connect(combo, &QComboBox::currentIndexChanged, &d, [this, combo, id]() {
				int index = -1;
				const Kind kind = Decode(combo->currentData().toString(), index);
				Set(id, kind, index);
			});
			grid->addWidget(combo, i, 1);
		}
		auto *hint = new QLabel("Вход на экране работает независимо от эфира: клип играет, камера идёт. "
					"Триггер «Экран: показать» переключает экран сам.",
					&d);
		hint->setWordWrap(true);
		root->addWidget(hint);
		auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
		QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
		root->addWidget(bb);
		d.resize(620, 120 + 40 * (int)screens.size());
		d.exec();
	}

private:
	static QString Encode(Kind kind, int index)
	{
		switch (kind) {
		case Kind::Program:
			return "program";
		case Kind::Preview:
			return "preview";
		case Kind::Input:
			return QString("input:%1").arg(index);
		default:
			return "off";
		}
	}
	static Kind Decode(const QString &v, int &index)
	{
		if (v == "program")
			return Kind::Program;
		if (v == "preview")
			return Kind::Preview;
		if (v.startsWith("input:")) {
			index = v.mid(6).toInt();
			return Kind::Input;
		}
		return Kind::Off;
	}
	static QScreen *Find(const QString &id)
	{
		for (QScreen *sc : QGuiApplication::screens())
			if (sc->name() == id)
				return sc;
		return nullptr;
	}

	/* keeps displays bound to their windows; Preview screens follow the preview */
	void Refresh()
	{
		for (auto &[id, w] : windows_) {
			if (chosen_[id].first == Kind::Preview)
				w->Display()->SetSource(sw_->InputScene(sw_->Preview()));
			w->Display()->Sync(w->isVisible());
		}
	}

	myobs::Switcher *sw_;
	std::function<QStringList()> names_;
	myobs::ScreenFeeds feeds_;
	std::map<QString, std::unique_ptr<ScreenWindow>> windows_;
	std::map<QString, std::pair<Kind, int>> chosen_;
	QTimer *timer_ = nullptr;
};
