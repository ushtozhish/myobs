/* MyOBS inputs panel.
 *
 * Tiles: live thumbnail, number, type, name; audio meter, fader and mute for inputs with sound.
 * Click = Preview, double click = on air by CUT, right button = properties, rename, tab, remove.
 * Tabs: «Все» plus own colour-coded tabs; an input belongs to one tab or none.
 * CUT / FADE with the fade length; + Клип / + Картинка / + Цвет.
 * Sound per input: fader, routing «кубики» M / A / B (buses Мастер-стрим, A, B) and AFV (only while
 * its picture is on air). «Микшер»: the whole routing matrix - inputs and buses to buses and to every
 * audio device (devices appear by themselves), with a delay per device.
 * Inputs, their sound settings and the tabs are saved and restored between starts. */
#pragma once

#include <obs.hpp>

#include "devices.hpp"
#include "display.hpp"
#include "props.hpp"
#include "switcher.hpp"
#include "thumbs.hpp"
#include "recorder.hpp"
#include "screens.hpp"

#include <QDateTime>
#include <QSignalBlocker>
#include <QDir>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QUrl>
#include "triggers_ui.hpp"

#include <QGuiApplication>
#include <QScreen>

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <vector>

/* ---- audio of one input: meter (engine audio thread -> atomic) and fader ---- */

class InputAudio {
public:
	explicit InputAudio(obs_source_t *source) : source_(source)
	{
		meter_ = obs_volmeter_create(OBS_FADER_LOG);
		obs_volmeter_attach_source(meter_, source);
		obs_volmeter_add_callback(meter_, OnLevels, this);
		fader_ = obs_fader_create(OBS_FADER_LOG);
		obs_fader_attach_source(fader_, source);
	}
	~InputAudio()
	{
		obs_volmeter_remove_callback(meter_, OnLevels, this);
		obs_volmeter_destroy(meter_);
		obs_fader_destroy(fader_);
	}
	/* highest peak since the last call, dBFS */
	float TakePeakDb()
	{
		float db = peak_.exchange(-100.0f);
		/* a clip renders its own sound: its meter comes from the router */
		const float lin = myobs::AudioBus::TakeInputPeak(source_);
		if (lin > 0.0f)
			db = std::max(db, 20.0f * std::log10(lin));
		return db;
	}
	float Deflection() const { return obs_fader_get_deflection(fader_); }
	void SetDeflection(float d) { obs_fader_set_deflection(fader_, d); }

private:
	static void OnLevels(void *param, const float *, const float *peak, const float *)
	{
		auto *self = static_cast<InputAudio *>(param);
		float p = std::max(peak[0], peak[1]);
		if (!std::isfinite(p))
			p = -100.0f;
		float prev = self->peak_.load();
		while (p > prev && !self->peak_.compare_exchange_weak(prev, p)) {
		}
	}
	obs_source_t *source_ = nullptr;
	obs_volmeter_t *meter_ = nullptr;
	obs_fader_t *fader_ = nullptr;
	std::atomic<float> peak_{-100.0f};
};

/* Vertical level bar, -60..0 dBFS, falling display and peak hold. */
class MeterBar : public QWidget {
public:
	explicit MeterBar(QWidget *parent) : QWidget(parent) { setFixedWidth(8); }
	void Update(InputAudio *audio)
	{
		const float p = audio ? audio->TakePeakDb() : -100.0f;
		shown_ = std::max(p, shown_ - 1.5f);
		if (p >= hold_) {
			hold_ = p;
			holdTicks_ = 30;
		} else if (--holdTicks_ <= 0) {
			hold_ = std::max(hold_ - 1.5f, shown_);
		}
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter g(this);
		g.fillRect(rect(), QColor("#0e0f11"));
		auto y = [this](float db) {
			return (int)(height() * (1.0 - (std::clamp(db, -60.0f, 0.0f) + 60.0) / 60.0));
		};
		const int top = y(shown_), y18 = y(-18.0f), y6 = y(-6.0f);
		if (top < height()) {
			const int a = std::max(top, y18);
			g.fillRect(QRect(0, a, width(), height() - a), QColor("#8a8f97"));
			if (top < y18) {
				const int b = std::max(top, y6);
				g.fillRect(QRect(0, b, width(), y18 - b), QColor("#f28c1e"));
			}
			if (top < y6)
				g.fillRect(QRect(0, top, width(), y6 - top), QColor("#e0312b"));
		}
		if (hold_ > -60.0f)
			g.fillRect(QRect(0, y(hold_), width(), 2), hold_ > -6.0f ? QColor("#e0312b") : QColor("#d8dade"));
	}

private:
	float shown_ = -100.0f;
	float hold_ = -100.0f;
	int holdTicks_ = 0;
};

/* ---- one tile ---- */

class InputTile : public QFrame {
public:
	static constexpr int kThumbW = 208, kThumbH = 117;

	InputTile(int index, obs_source_t *scene, bool hasAudio, QWidget *parent)
		: QFrame(parent),
		  index_(index),
		  scene_(scene)
	{
		setObjectName("tile");
		setCursor(Qt::PointingHandCursor);
		auto *outer = new QHBoxLayout(this);
		outer->setContentsMargins(4, 4, 4, 4);
		outer->setSpacing(4);
		auto *left = new QVBoxLayout();
		left->setSpacing(3);
		thumb_ = new SourceDisplay(SourceDisplay::Mode::Thumbnail, this);
		thumb_->setFixedSize(kThumbW, kThumbH);
		thumb_->SetSource(scene);
		thumb_->installEventFilter(this);
		left->addWidget(thumb_);
		number_ = new QLabel(this);
		number_->setObjectName("tileNumber");
		left->addWidget(number_);
		name_ = new QLabel(this);
		name_->setObjectName("tileName");
		left->addWidget(name_);
		if (hasAudio) {
			auto *row = new QHBoxLayout();
			row->setSpacing(4);
			fader_ = new QSlider(Qt::Horizontal, this);
			fader_->setRange(0, 1000);
			fader_->setToolTip("Громкость входа");
			static const char *busNames[3] = {"M", "A", "B"};
			static const char *busTips[3] = {"Шина «Мастер-стрим»: стрим и запись эфира", "Шина A", "Шина B"};
			for (int b = 0; b < 3; b++) {
				cubes_[b] = new QPushButton(busNames[b], this);
				cubes_[b]->setObjectName("cube");
				cubes_[b]->setCheckable(true);
				cubes_[b]->setFixedSize(20, 20);
				cubes_[b]->setToolTip(busTips[b]);
			}
			afv_ = new QPushButton("AFV", this);
			afv_->setObjectName("afv");
			afv_->setCheckable(true);
			afv_->setFixedSize(36, 20);
			afv_->setToolTip("Звук следует за картинкой: звучит, только пока вход в эфире; на FADE — плавно");
			row->addWidget(fader_, 1);
			for (QPushButton *c : cubes_)
				row->addWidget(c);
			row->addWidget(afv_);
			left->addLayout(row);
		}
		outer->addLayout(left);
		if (hasAudio) {
			meter_ = new MeterBar(this);
			outer->addWidget(meter_);
		}
		setToolTip("Клик — в Preview\nДвойной клик — сразу в эфир (CUT)\nПравая кнопка — свойства, вкладка, удалить");
	}

	void SetText(const QString &type, const QString &name, const QColor &tabColor)
	{
		number_->setText(QString("%1  ·  %2").arg(index_ + 1).arg(type));
		name_->setText(fontMetrics().elidedText(name, Qt::ElideMiddle, kThumbW));
		tabColor_ = tabColor;
	}

	void SetState(bool program, bool preview)
	{
		const char *state = program ? "program" : preview ? "preview" : "idle";
		if (state_ == state)
			return;
		state_ = state;
		setProperty("state", state);
		style()->unpolish(this);
		style()->polish(this);
		update();
	}

	void Sync(bool visible) { thumb_->Sync(visible); }
	QWidget *Thumb() const { return thumb_; }
	obs_source_t *Scene() const { return scene_; }
	QSlider *Fader() const { return fader_; }
	QPushButton *Cube(int bus) const { return cubes_[bus]; }
	QPushButton *Afv() const { return afv_; }
	MeterBar *Meter() const { return meter_; }

	std::function<void(int)> onClick, onDoubleClick, onMenu;

protected:
	void paintEvent(QPaintEvent *e) override
	{
		QFrame::paintEvent(e);
		if (tabColor_.isValid()) { /* tab colour: a stripe along the bottom edge */
			QPainter g(this);
			g.fillRect(QRect(2, height() - 5, width() - 4, 3), tabColor_);
		}
	}
	bool eventFilter(QObject *obj, QEvent *e) override
	{
		if (obj == thumb_) {
			if (e->type() == QEvent::MouseButtonPress)
				mousePressEvent(static_cast<QMouseEvent *>(e));
			else if (e->type() == QEvent::MouseButtonDblClick)
				mouseDoubleClickEvent(static_cast<QMouseEvent *>(e));
			else
				return false;
			return true;
		}
		return QFrame::eventFilter(obj, e);
	}
	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton && onClick)
			onClick(index_);
		else if (e->button() == Qt::RightButton && onMenu)
			onMenu(index_);
	}
	void mouseDoubleClickEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton && onDoubleClick)
			onDoubleClick(index_);
	}

private:
	int index_;
	obs_source_t *scene_;
	SourceDisplay *thumb_ = nullptr;
	QLabel *number_ = nullptr;
	QLabel *name_ = nullptr;
	QSlider *fader_ = nullptr;
	QPushButton *cubes_[3] = {nullptr, nullptr, nullptr};
	QPushButton *afv_ = nullptr;
	MeterBar *meter_ = nullptr;
	QColor tabColor_;
	const char *state_ = "";
};

/* ---- panel ---- */

class InputsPanel : public QWidget {
public:
	explicit InputsPanel(myobs::Switcher *sw, QWidget *parent) : QWidget(parent), sw_(sw)
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(0, 0, 0, 0);
		root->setSpacing(6);

		auto *bar = new QHBoxLayout();
		bar->setSpacing(6);
		auto *cut = new QPushButton("CUT", this);
		cut->setObjectName("cut");
		auto *fade = new QPushButton("FADE", this);
		fade->setObjectName("fade");
		fadeMs_ = new QSpinBox(this);
		fadeMs_->setRange(40, 10000);
		fadeMs_->setSingleStep(100);
		fadeMs_->setSuffix(" мс");
		fadeMs_->setValue(QSettings().value("switcher/fadeMs", 500).toInt());
		connect(fadeMs_, &QSpinBox::valueChanged, this, [](int v) { QSettings().setValue("switcher/fadeMs", v); });
		connect(cut, &QPushButton::clicked, this, [this]() { sw_->Cut(); });
		connect(fade, &QPushButton::clicked, this, [this]() { sw_->Fade((uint32_t)fadeMs_->value()); });
		bar->addWidget(cut);
		bar->addWidget(fade);
		bar->addWidget(fadeMs_);
		bar->addSpacing(18);
		auto *addClip = new QPushButton("+ Клип", this);
		auto *addImage = new QPushButton("+ Картинка", this);
		auto *addColor = new QPushButton("+ Цвет", this);
		auto *addCamera = new QPushButton("+ Камера", this);
		addCamera->setToolTip("Камера или карта захвата (DirectShow)");
		auto *addDeckLink = new QPushButton("+ DeckLink", this);
		connect(addCamera, &QPushButton::clicked, this, [this]() { AddDevice("camera", "Камера"); });
		connect(addDeckLink, &QPushButton::clicked, this, [this]() { AddDevice("decklink", "DeckLink"); });
		auto *addNdi = new QPushButton("+ NDI", this);
		connect(addNdi, &QPushButton::clicked, this, [this]() { AddDevice("ndi", "NDI"); });
		auto *addCall = new QPushButton("+ Звонок", this);
		addCall->setToolTip("Прямой звонок (VDO.Ninja): гость подключается по ссылке из браузера или телефона");
		connect(addCall, &QPushButton::clicked, this, [this]() { AddDevice("call", "Звонок"); });
		connect(addClip, &QPushButton::clicked, this, [this]() { AddClipDialog(); });
		connect(addImage, &QPushButton::clicked, this, [this]() { AddImageDialog(); });
		connect(addColor, &QPushButton::clicked, this, [this]() { AddColorDialog(); });
		bar->addWidget(addClip);
		bar->addWidget(addImage);
		bar->addWidget(addColor);
		bar->addWidget(addCamera);
		bar->addWidget(addDeckLink);
		bar->addWidget(addNdi);
		bar->addWidget(addCall);
		bar->addSpacing(18);
		auto *mixer = new QPushButton("Микшер", this);
		mixer->setToolTip("Маршрутизация звука: входы и шины → шины и звуковые устройства");
		connect(mixer, &QPushButton::clicked, this, [this]() { MixerDialog(); });
		bar->addWidget(mixer);
		auto *screensBtn = new QPushButton("Экраны", this);
		screensBtn->setToolTip("Вывод входов, Program или Preview во весь экран на мониторы и LED");
		connect(screensBtn, &QPushButton::clicked, this, [this]() { screens_->Dialog(this); });
		bar->addWidget(screensBtn);
		bar->addSpacing(18);
		recordBtn_ = new QPushButton("● Запись", this);
		recordBtn_->setObjectName("record");
		recordBtn_->setCheckable(true);
		recordBtn_->setToolTip("Запись эфира в рабочем кодеке (H.264, ключевой кадр каждые 10 кадров, без B-кадров).\n"
				       "Правая кнопка — дорожки звука и папка.");
		recordBtn_->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(recordBtn_, &QPushButton::toggled, this, [this](bool on) { ToggleRecord(on); });
		connect(recordBtn_, &QPushButton::customContextMenuRequested, this, [this]() { RecordMenu(); });
		bar->addWidget(recordBtn_);
		bar->addStretch();
		status_ = new QLabel(this);
		status_->setObjectName("switchStatus");
		bar->addWidget(status_);
		root->addLayout(bar);

		tabs_ = new QTabBar(this);
		tabs_->setObjectName("inputTabs");
		tabs_->setExpanding(false);
		tabs_->setDrawBase(false);
		tabs_->setContextMenuPolicy(Qt::CustomContextMenu);
		tabs_->setToolTip("Правая кнопка — новая вкладка, переименовать, цвет, удалить");
		connect(tabs_, &QTabBar::currentChanged, this, [this](int) {
			if (restoringTabs_)
				return;
			QSettings().setValue("inputs/currentTab", CurrentTab());
			RebuildTiles();
		});
		connect(tabs_, &QTabBar::customContextMenuRequested, this,
			[this](const QPoint &p) { TabMenu(tabs_->tabAt(p)); });
		root->addWidget(tabs_);

		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		host_ = new QWidget(scroll);
		host_->setObjectName("tilesHost");
		grid_ = new QGridLayout(host_);
		grid_->setContentsMargins(0, 0, 0, 0);
		grid_->setSpacing(6);
		grid_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
		scroll->setWidget(host_);
		root->addWidget(scroll, 1);

		RestoreTabs();
		RestoreInputs();
		RestoreBusSends();
		screens_ = std::make_unique<ScreenOutputs>(sw_, [this]() {
			QStringList n;
			for (const Input &in : inputs_)
				n << in.name;
			return n;
		});
		screens_->Restore();
		/* trigger «Экран: показать» comes from the engine thread */
		sw_->SetScreenHandler([this](const std::string &screen, int index) {
			const QString id = QString::fromStdString(screen);
			QMetaObject::invokeMethod(this, [this, id, index]() { screens_->Set(id, ScreenOutputs::Kind::Input, index); },
						  Qt::QueuedConnection);
		});
		RebuildTiles();
	}

	~InputsPanel() override
	{
		sw_->SetScreenHandler(nullptr);
		recorder_.Stop();
		screens_.reset();
		for (InputTile *t : tiles_)
			delete t;
		tiles_.clear();
		Thumbs::Get().SetScenes({});
	}

	/* Window timer, 25 Hz: tile colours, meters, switch status. */
	void Refresh()
	{
		const int program = sw_->Program(), preview = sw_->Preview();
		for (size_t t = 0; t < tiles_.size(); t++) {
			const int i = tileInput_[t];
			tiles_[t]->SetState(i == program, i == preview);
			if (tiles_[t]->Meter())
				tiles_[t]->Meter()->Update(inputs_[i].audio.get());
		}
		if (recorder_.Active()) {
			const qint64 sec = (QDateTime::currentMSecsSinceEpoch() - recordStart_) / 1000;
			recordBtn_->setText(QString("● %1:%2:%3")
						    .arg(sec / 3600, 2, 10, QChar('0'))
						    .arg(sec / 60 % 60, 2, 10, QChar('0'))
						    .arg(sec % 60, 2, 10, QChar('0')));
		}
		const size_t pending = sw_->PendingRequests();
		status_->setText(sw_->Transitioning() ? "переход…" : pending ? QString("в очереди: %1").arg(pending) : "");
	}

	/* Demo / screenshots: tile thumbnail widgets with their scenes; select a tab by name. */
	std::vector<std::pair<QWidget *, obs_source_t *>> ThumbTargets() const
	{
		std::vector<std::pair<QWidget *, obs_source_t *>> out;
		for (InputTile *t : tiles_)
			out.push_back({t->Thumb(), t->Scene()});
		return out;
	}
	void SelectTab(const QString &name)
	{
		for (int i = 0; i < tabs_->count(); i++)
			if (tabs_->tabText(i) == name)
				tabs_->setCurrentIndex(i);
	}

	/* Window timer, 2 Hz: keep tile displays bound to their windows. */
	void SyncDisplays(bool visible)
	{
		for (InputTile *t : tiles_)
			t->Sync(visible);
	}

protected:
	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		if (Columns() != columns_)
			RebuildTiles();
	}

private:
	struct Input {
		QString type; // clip | image | color
		QString name;
		QString tab; // empty = no tab
		uint32_t routes = 0x1; // buses: bit 0 = Мастер-стрим, A, B (new inputs: Мастер-стрим only)
		QStringList devices;   // audio devices this input is sent to
		bool afv = true;
		OBSSourceAutoRelease source;
		std::unique_ptr<InputAudio> audio;
	};
	struct Tab {
		QString name;
		QColor color;
	};

	static QString TypeTitle(const QString &type)
	{
		return type == "clip"       ? "Клип"
		       : type == "image"    ? "Картинка"
		       : type == "color"    ? "Цвет"
		       : type == "camera"   ? "Камера"
		       : type == "decklink" ? "DeckLink"
		       : type == "ndi"      ? "NDI"
		       : type == "call"     ? "Звонок"
					    : type;
	}

	static const char *SourceId(const QString &type)
	{
		return type == "clip"       ? "myobs_clip"
		       : type == "image"    ? "image_source"
		       : type == "camera"   ? "dshow_input"
		       : type == "decklink" ? "decklink-input"
		       : type == "ndi"      ? "myobs_ndi"
		       : type == "call"     ? "vdoninja_source"
					    : "color_source_v3";
	}

	int Columns() const { return std::max(1, (width() - 4) / (InputTile::kThumbW + 36)); }

	QString CurrentTab() const
	{
		const int i = tabs_->currentIndex();
		return i <= 0 ? QString() : tabs_->tabText(i);
	}

	QColor TabColor(const QString &name) const
	{
		for (const Tab &t : tabList_)
			if (t.name == name)
				return t.color;
		return QColor();
	}

	bool Add(const QString &type, const QString &name, obs_data_t *settings, const QString &tab, double volume,
		 uint32_t routes, bool afv, bool save = true, const QStringList &devices = {})
	{
		OBSSourceAutoRelease src = obs_source_create(SourceId(type), name.toUtf8().constData(), settings, nullptr);
		if (!src) {
			QMessageBox::warning(this, "MyOBS", QString("Не удалось создать вход «%1».").arg(name));
			return false;
		}
		obs_source_set_volume(src, (float)volume);
		const bool hasAudio = myobs::SourceHasAudio(src);
		const int index = sw_->AddInput(src, name.toStdString());
		for (int b = 0; b < 3; b++)
			sw_->SetAudioRoute(index, b, (routes >> b) & 1u);
		for (const QString &d : devices) {
			EnsureSink(d);
			sw_->Audio().SetInputToSink(sw_->InputSource(index), d.toStdString(), true);
		}
		sw_->SetAudioAfv(index, afv);
		Input in{type, name, tab, routes, devices, afv, std::move(src), nullptr};
		if (hasAudio)
			in.audio = std::make_unique<InputAudio>(in.source);
		inputs_.push_back(std::move(in));
		if (save) {
			RebuildTiles();
			SaveInputs();
		}
		return true;
	}

	void AddFiles(const QString &type, const QStringList &files)
	{
		for (const QString &f : files) {
			OBSDataAutoRelease s = obs_data_create();
			obs_data_set_string(s, "file", f.toUtf8().constData());
			Add(type, QFileInfo(f).completeBaseName(), s, CurrentTab(), 1.0, 0x1, true);
		}
	}

	void AddClipDialog()
	{
		const QStringList files = QFileDialog::getOpenFileNames(
			this, "Клипы", QSettings().value("paths/clips").toString(),
			"Видео (*.mp4 *.mov *.mkv *.avi *.mxf *.m4v *.ts *.mts *.webm);;Все файлы (*.*)");
		if (!files.isEmpty())
			QSettings().setValue("paths/clips", QFileInfo(files.first()).absolutePath());
		AddFiles("clip", files);
	}

	void AddImageDialog()
	{
		const QStringList files = QFileDialog::getOpenFileNames(
			this, "Картинки", QSettings().value("paths/images").toString(),
			"Картинки (*.png *.jpg *.jpeg *.bmp *.tga *.gif *.webp);;Все файлы (*.*)");
		if (!files.isEmpty())
			QSettings().setValue("paths/images", QFileInfo(files.first()).absolutePath());
		AddFiles("image", files);
	}

	void AddColorDialog()
	{
		const QColor c = QColorDialog::getColor(Qt::black, this, "Цвет");
		if (!c.isValid())
			return;
		OBSDataAutoRelease s = obs_data_create();
		SetColor(s, c);
		Add("color", QString("Цвет %1").arg(c.name()), s, CurrentTab(), 1.0, 0x1, true);
	}

	/* Live device input: created, then its settings dialog; Cancel on creation removes it again. */
	void AddDevice(const QString &type, const QString &base)
	{
		if (!obs_source_get_display_name(SourceId(type))) {
			QMessageBox::warning(this, "MyOBS",
					     type == "decklink"
						     ? QString("DeckLink недоступен: не установлен драйвер Blackmagic Desktop Video.")
						     : QString("Модуль входа «%1» не загружен.").arg(TypeTitle(type)));
			return;
		}
		int n = 1;
		for (const Input &in : inputs_)
			if (in.type == type)
				n++;
		OBSDataAutoRelease s = obs_data_create();
		if (!Add(type, QString("%1 %2").arg(base).arg(n), s, CurrentTab(), 1.0, 0x1, true, false))
			return;
		const int index = (int)inputs_.size() - 1;
		PropertiesDialog d(inputs_[index].source, QString("Свойства — %1").arg(inputs_[index].name), this);
		if (d.exec() == QDialog::Accepted) {
			RefreshAudio(index);
			RebuildTiles();
			SaveInputs();
		} else if (sw_->RemoveInput(index)) {
			ClearTiles();
			inputs_.erase(inputs_.begin() + index);
			RebuildTiles();
			SaveInputs();
		}
	}

	/* A device may get or lose sound after its settings change. */
	void RefreshAudio(int index)
	{
		Input &in = inputs_[index];
		const bool hasAudio = myobs::SourceHasAudio(in.source);
		if (hasAudio && !in.audio) {
			ClearTiles();
			in.audio = std::make_unique<InputAudio>(in.source);
			RebuildTiles();
		}
	}

	static void SetColor(obs_data_t *s, const QColor &c)
	{
		obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		/* OBS colour is ABGR */
		const long long abgr = (0xFFll << 24) | ((long long)c.blue() << 16) | ((long long)c.green() << 8) | c.red();
		obs_data_set_int(s, "color", abgr);
		obs_data_set_int(s, "width", ovi.base_width ? ovi.base_width : 1920);
		obs_data_set_int(s, "height", ovi.base_height ? ovi.base_height : 1080);
	}

	void ClearTiles()
	{
		for (InputTile *t : tiles_)
			delete t;
		tiles_.clear();
		tileInput_.clear();
	}

	void RebuildTiles()
	{
		ClearTiles();
		columns_ = Columns();
		const QString tab = CurrentTab();
		std::vector<obs_source_t *> scenes;
		int n = 0;
		for (int i = 0; i < (int)inputs_.size(); i++) {
			Input &in = inputs_[i];
			if (!tab.isEmpty() && in.tab != tab)
				continue;
			obs_source_t *scene = sw_->InputScene(i);
			scenes.push_back(scene);
			auto *t = new InputTile(i, scene, in.audio != nullptr, host_);
			t->SetText(TypeTitle(in.type), in.name, TabColor(in.tab));
			t->onClick = [this](int idx) { sw_->SetPreview(idx); };
			t->onDoubleClick = [this](int idx) {
				sw_->SetPreview(idx);
				sw_->Cut();
			};
			t->onMenu = [this](int idx) {
				/* the menu may rebuild the tiles: run it after this event is done */
				QMetaObject::invokeMethod(this, [this, idx]() { Menu(idx); }, Qt::QueuedConnection);
			};
			if (in.audio) {
				t->Fader()->setValue((int)std::lround(in.audio->Deflection() * 1000.0f));
				for (int b = 0; b < 3; b++) {
					t->Cube(b)->setChecked((in.routes >> b) & 1u);
					connect(t->Cube(b), &QPushButton::toggled, this, [this, i, b](bool v) { SetRoute(i, b, v); });
				}
				t->Afv()->setChecked(in.afv);
				InputAudio *audio = in.audio.get();
				connect(t->Fader(), &QSlider::valueChanged, this, [audio](int v) { audio->SetDeflection(v / 1000.0f); });
				connect(t->Fader(), &QSlider::sliderReleased, this, [this]() { SaveInputs(); });
				connect(t->Afv(), &QPushButton::toggled, this, [this, i](bool v) {
					inputs_[i].afv = v;
					sw_->SetAudioAfv(i, v);
					SaveInputs();
				});
			}
			grid_->addWidget(t, n / columns_, n % columns_);
			tiles_.push_back(t);
			tileInput_.push_back(i);
			n++;
		}
		Thumbs::Get().SetScenes(scenes);
		Refresh();
	}

	void Menu(int index)
	{
		if (index < 0 || index >= (int)inputs_.size())
			return;
		QMenu menu(this);
		QAction *props = menu.addAction("Свойства…");
		QAction *rename = menu.addAction("Переименовать…");
		QAction *trig = menu.addAction("Триггеры…");
		QMenu *tabMenu = menu.addMenu("Вкладка");
		auto *group = new QActionGroup(tabMenu);
		QAction *none = tabMenu->addAction("Без вкладки");
		none->setCheckable(true);
		none->setChecked(inputs_[index].tab.isEmpty());
		group->addAction(none);
		std::vector<std::pair<QAction *, QString>> tabActions;
		for (const Tab &t : tabList_) {
			QAction *a = tabMenu->addAction(t.name);
			a->setCheckable(true);
			a->setChecked(inputs_[index].tab == t.name);
			group->addAction(a);
			tabActions.push_back({a, t.name});
		}
		tabMenu->addSeparator();
		QAction *newTab = tabMenu->addAction("Новая вкладка…");
		menu.addSeparator();
		QAction *remove = menu.addAction("Удалить");
		QAction *chosen = menu.exec(QCursor::pos());
		if (!chosen)
			return;
		if (chosen == props) {
			Properties(index);
		} else if (chosen == trig) {
			EditTriggers(index);
		} else if (chosen == rename) {
			Rename(index);
		} else if (chosen == remove) {
			Remove(index);
		} else if (chosen == none) {
			SetInputTab(index, QString());
		} else if (chosen == newTab) {
			const QString name = AddTab();
			if (!name.isEmpty())
				SetInputTab(index, name);
		} else {
			for (auto &[a, name] : tabActions)
				if (a == chosen)
					SetInputTab(index, name);
		}
	}

	void SetInputTab(int index, const QString &tab)
	{
		inputs_[index].tab = tab;
		RebuildTiles();
		SaveInputs();
	}

	void Rename(int index)
	{
		bool ok = false;
		const QString name =
			QInputDialog::getText(this, "Переименовать", "Название", QLineEdit::Normal, inputs_[index].name, &ok)
				.trimmed();
		if (!ok || name.isEmpty())
			return;
		inputs_[index].name = name;
		obs_source_set_name(inputs_[index].source, name.toUtf8().constData());
		RebuildTiles();
		SaveInputs();
	}

	void Properties(int index)
	{
		Input &in = inputs_[index];
		OBSDataAutoRelease s = obs_source_get_settings(in.source);
		if (in.type == "color") {
			const long long abgr = obs_data_get_int(s, "color");
			const QColor c = QColorDialog::getColor(
				QColor((int)(abgr & 0xFF), (int)((abgr >> 8) & 0xFF), (int)((abgr >> 16) & 0xFF)), this, "Цвет");
			if (!c.isValid())
				return;
			SetColor(s, c);
			obs_source_update(in.source, s);
			SaveInputs();
			return;
		}
		if (in.type != "clip" && in.type != "image") {
			PropertiesDialog pd(in.source, QString("Свойства — %1").arg(in.name), this);
			if (pd.exec() == QDialog::Accepted) {
				RefreshAudio(index);
				SaveInputs();
			}
			return;
		}
		QDialog d(this);
		d.setWindowTitle(QString("Свойства — %1").arg(in.name));
		d.setMinimumWidth(520);
		auto *f = new QFormLayout(&d);
		auto *file = new QLineEdit(QString::fromUtf8(obs_data_get_string(s, "file")), &d);
		auto *browse = new QPushButton("Обзор…", &d);
		auto *row = new QHBoxLayout();
		row->addWidget(file, 1);
		row->addWidget(browse);
		f->addRow("Файл", row);
		connect(browse, &QPushButton::clicked, &d, [&]() {
			const QString p = QFileDialog::getOpenFileName(&d, "Файл", QFileInfo(file->text()).absolutePath());
			if (!p.isEmpty())
				file->setText(p);
		});
		QComboBox *offair = nullptr;
		QCheckBox *loop = nullptr;
		if (in.type == "clip") {
			offair = new QComboBox(&d);
			offair->addItem("Пауза, продолжить с того же кадра", 0);
			offair->addItem("С начала при выходе в эфир", 1);
			offair->addItem("Не останавливать проигрывание", 2);
			offair->setCurrentIndex(std::max(0, offair->findData((int)obs_data_get_int(s, "offair"))));
			f->addRow("Вне эфира", offair);
			loop = new QCheckBox("Повтор", &d);
			loop->setChecked(obs_data_get_bool(s, "loop"));
			f->addRow("", loop);
		}
		auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
		f->addRow(bb);
		connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
		connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
		if (d.exec() != QDialog::Accepted)
			return;
		obs_data_set_string(s, "file", file->text().toUtf8().constData());
		if (offair)
			obs_data_set_int(s, "offair", offair->currentData().toInt());
		if (loop)
			obs_data_set_bool(s, "loop", loop->isChecked());
		obs_source_update(in.source, s);
		SaveInputs();
	}

	void Remove(int index)
	{
		if (!sw_->RemoveInput(index)) {
			QMessageBox::information(this, "MyOBS", "Вход сейчас в эфире или в переходе — его нельзя удалить.");
			return;
		}
		ClearTiles(); /* tiles use the input's audio and display: drop them first */
		inputs_.erase(inputs_.begin() + index);
		screens_->InputRemoved(index);
		RebuildTiles();
		SaveInputs();
	}

	/* ---- recording ---- */

	static QString RecordFolder()
	{
		const QString def = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/MyOBS";
		return QSettings().value("record/folder", def).toString();
	}

	void ToggleRecord(bool on)
	{
		if (!on) {
			recorder_.Stop();
			recordBtn_->setText("● Запись");
			return;
		}
		QDir().mkpath(RecordFolder());
		myobs::RecordSettings rs;
		rs.path = (RecordFolder() + "/MyOBS_" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") +
			   ".mkv")
				  .toStdString();
		rs.audioTracks = (uint32_t)QSettings().value("record/tracks", 1).toInt();
		if (!recorder_.Start(rs)) {
			QMessageBox::warning(this, "MyOBS",
					     QString("Запись не началась: %1").arg(QString::fromStdString(recorder_.Error())));
			const QSignalBlocker block(recordBtn_);
			recordBtn_->setChecked(false);
			return;
		}
		recordStart_ = QDateTime::currentMSecsSinceEpoch();
	}

	void RecordMenu()
	{
		QMenu menu(this);
		const uint32_t tracks = (uint32_t)QSettings().value("record/tracks", 1).toInt();
		static const char *names[3] = {"Дорожка: Мастер-стрим", "Дорожка: шина A", "Дорожка: шина B"};
		QAction *acts[3];
		for (int b = 0; b < 3; b++) {
			acts[b] = menu.addAction(QString::fromUtf8(names[b]));
			acts[b]->setCheckable(true);
			acts[b]->setChecked((tracks >> b) & 1u);
			acts[b]->setEnabled(!recorder_.Active());
		}
		menu.addSeparator();
		QAction *folder = menu.addAction("Папка записей: " + RecordFolder());
		QAction *open = menu.addAction("Открыть папку");
		QAction *chosen = menu.exec(QCursor::pos());
		if (!chosen)
			return;
		for (int b = 0; b < 3; b++)
			if (chosen == acts[b]) {
				uint32_t t = chosen->isChecked() ? (tracks | (1u << b)) : (tracks & ~(1u << b));
				if (!t)
					t = 1; /* at least one track */
				QSettings().setValue("record/tracks", (int)t);
			}
		if (chosen == folder) {
			const QString dir = QFileDialog::getExistingDirectory(this, "Папка записей", RecordFolder());
			if (!dir.isEmpty())
				QSettings().setValue("record/folder", dir);
		} else if (chosen == open) {
			QDir().mkpath(RecordFolder());
			QDesktopServices::openUrl(QUrl::fromLocalFile(RecordFolder()));
		}
	}

	/* ---- triggers ---- */

	std::vector<triggersui::InputRef> InputRefs() const
	{
		std::vector<triggersui::InputRef> refs;
		for (int i = 0; i < (int)inputs_.size(); i++)
			refs.push_back({inputs_[i].name, sw_->InputSource(i)});
		return refs;
	}

	/* screens for «Экран: показать» and screen outputs: (id, title) */
	static std::vector<std::pair<QString, QString>> ScreenList()
	{
		std::vector<std::pair<QString, QString>> out;
		const auto screens = QGuiApplication::screens();
		for (int i = 0; i < screens.size(); i++) {
			QScreen *sc = screens[i];
			out.push_back({sc->name(), QString("Экран %1: %2 (%3×%4)")
							   .arg(i + 1)
							   .arg(sc->model().isEmpty() ? sc->name() : sc->model())
							   .arg(sc->size().width() * sc->devicePixelRatio())
							   .arg(sc->size().height() * sc->devicePixelRatio())});
		}
		return out;
	}

	void EditTriggers(int index)
	{
		std::vector<myobs::Trigger> t = sw_->Triggers(index);
		if (!triggersui::Edit(this, inputs_[index].name, t, InputRefs(), ScreenList()))
			return;
		sw_->SetTriggers(index, t);
		SaveInputs();
	}

	/* ---- sound routing ---- */

	static uint32_t RoutesFrom(const QJsonObject &o)
	{
		if (o.contains("routes"))
			return (uint32_t)o["routes"].toInt(0x1);
		/* older saves: «ЭФИР» button = Мастер-стрим */
		return o["audioOnAir"].toBool(true) ? 0x1u : 0x0u;
	}

	void SetRoute(int i, int bus, bool on)
	{
		inputs_[i].routes = on ? (inputs_[i].routes | (1u << bus)) : (inputs_[i].routes & ~(1u << bus));
		sw_->SetAudioRoute(i, bus, on);
		SaveInputs();
	}

	double DeviceDelay(const QString &id) const
	{
		return QJsonDocument::fromJson(QSettings().value("audio/deviceDelay").toByteArray()).object()[id].toDouble(40.0);
	}

	void SetDeviceDelay(const QString &id, double ms)
	{
		QJsonObject o = QJsonDocument::fromJson(QSettings().value("audio/deviceDelay").toByteArray()).object();
		o[id] = ms;
		QSettings().setValue("audio/deviceDelay", QJsonDocument(o).toJson(QJsonDocument::Compact));
		auto it = sinks_.find(id);
		if (it != sinks_.end())
			it->second->SetDelayMs(ms);
	}

	/* a device is opened the first time something is sent to it */
	void EnsureSink(const QString &id)
	{
		if (sinks_.count(id))
			return;
		auto sink = std::make_shared<myobs::DeviceSink>(id.toStdString(), DeviceDelay(id));
		sinks_[id] = sink;
		sw_->Audio().AddSink(id.toStdString(), sink);
	}

	void SetInputDevice(int i, const QString &id, bool on)
	{
		if (on) {
			EnsureSink(id);
			if (!inputs_[i].devices.contains(id))
				inputs_[i].devices << id;
		} else {
			inputs_[i].devices.removeAll(id);
		}
		sw_->Audio().SetInputToSink(sw_->InputSource(i), id.toStdString(), on);
		SaveInputs();
	}

	QJsonObject BusSends() const
	{
		return QJsonDocument::fromJson(QSettings().value("audio/busToDevice").toByteArray()).object();
	}

	void SetBusDevice(int bus, const QString &id, bool on)
	{
		if (on)
			EnsureSink(id);
		sw_->Audio().SetBusToSink(bus, id.toStdString(), on);
		QJsonObject o = BusSends();
		uint32_t mask = (uint32_t)o[id].toInt(0);
		mask = on ? (mask | (1u << bus)) : (mask & ~(1u << bus));
		o[id] = (int)mask;
		QSettings().setValue("audio/busToDevice", QJsonDocument(o).toJson(QJsonDocument::Compact));
	}

	void RestoreBusSends()
	{
		const QJsonObject o = BusSends();
		for (auto it = o.begin(); it != o.end(); ++it) {
			const uint32_t mask = (uint32_t)it.value().toInt(0);
			if (!mask)
				continue;
			EnsureSink(it.key());
			for (int b = 0; b < 3; b++)
				if (mask & (1u << b))
					sw_->Audio().SetBusToSink(b, it.key().toStdString(), true);
		}
	}

	/* «Микшер»: routing matrix with cubes. Rows: inputs with sound, then the buses.
	 * Columns: buses M / A / B, then every playback device (each device appears by itself). */
	void MixerDialog()
	{
		const std::vector<myobs::AudioDeviceInfo> devices = myobs::ListOutputDevices();
		QDialog d(this);
		d.setWindowTitle("Микшер — маршрутизация звука");
		auto *root = new QVBoxLayout(&d);
		auto *scroll = new QScrollArea(&d);
		scroll->setWidgetResizable(true);
		auto *host = new QWidget(scroll);
		auto *grid = new QGridLayout(host);
		grid->setHorizontalSpacing(6);
		grid->setVerticalSpacing(4);
		scroll->setWidget(host);
		root->addWidget(scroll, 1);
		static const char *busNames[3] = {"M\nМастер-стрим", "A", "B"};
		const int devCol0 = 4;
		/* headers */
		for (int b = 0; b < 3; b++) {
			auto *h = new QLabel(QString::fromUtf8(busNames[b]).replace("\\n", "\n"), host);
			h->setAlignment(Qt::AlignCenter);
			grid->addWidget(h, 0, 1 + b);
		}
		for (size_t k = 0; k < devices.size(); k++) {
			const QString id = QString::fromStdString(devices[k].id);
			auto *box = new QWidget(host);
			auto *v = new QVBoxLayout(box);
			v->setContentsMargins(0, 0, 0, 0);
			auto *name = new QLabel(QString::fromStdString(devices[k].name), box);
			name->setWordWrap(true);
			name->setMaximumWidth(140);
			name->setAlignment(Qt::AlignCenter);
			auto *delay = new QSpinBox(box);
			delay->setRange(10, 1000);
			delay->setSuffix(" мс");
			delay->setValue((int)DeviceDelay(id));
			delay->setToolTip("Задержка устройства: выравнивает звук в зале с экранами");
			connect(delay, &QSpinBox::valueChanged, this, [this, id](int v) { SetDeviceDelay(id, v); });
			v->addWidget(name);
			v->addWidget(delay);
			grid->addWidget(box, 0, devCol0 + (int)k);
		}
		auto cube = [&](bool on, auto onToggle) {
			auto *c = new QPushButton(host);
			c->setObjectName("cube");
			c->setCheckable(true);
			c->setChecked(on);
			c->setFixedSize(26, 26);
			connect(c, &QPushButton::toggled, this, onToggle);
			return c;
		};
		int row = 1;
		for (int i = 0; i < (int)inputs_.size(); i++) {
			if (!inputs_[i].audio)
				continue;
			grid->addWidget(new QLabel(inputs_[i].name, host), row, 0);
			for (int b = 0; b < 3; b++)
				grid->addWidget(cube((inputs_[i].routes >> b) & 1u, [this, i, b](bool v) {
							SetRoute(i, b, v);
							RebuildTiles();
						}),
						row, 1 + b, Qt::AlignCenter);
			for (size_t k = 0; k < devices.size(); k++) {
				const QString id = QString::fromStdString(devices[k].id);
				grid->addWidget(cube(inputs_[i].devices.contains(id),
						     [this, i, id](bool v) { SetInputDevice(i, id, v); }),
						row, devCol0 + (int)k, Qt::AlignCenter);
			}
			row++;
		}
		auto *sep = new QLabel("Шины → устройства", host);
		sep->setObjectName("mixerSection");
		grid->addWidget(sep, row++, 0, 1, 4);
		static const char *busRows[3] = {"Шина M (Мастер-стрим)", "Шина A", "Шина B"};
		const QJsonObject sends = BusSends();
		for (int b = 0; b < 3; b++) {
			grid->addWidget(new QLabel(QString::fromUtf8(busRows[b]), host), row, 0);
			for (size_t k = 0; k < devices.size(); k++) {
				const QString id = QString::fromStdString(devices[k].id);
				const bool on = ((uint32_t)sends[id].toInt(0) >> b) & 1u;
				grid->addWidget(cube(on, [this, b, id](bool v) { SetBusDevice(b, id, v); }), row,
						devCol0 + (int)k, Qt::AlignCenter);
			}
			row++;
		}
		if (devices.empty())
			grid->addWidget(new QLabel("Звуковые устройства вывода не найдены.", host), row++, 0, 1, 4);
		grid->setRowStretch(row, 1);
		grid->setColumnStretch(devCol0 + (int)devices.size(), 1);
		auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
		connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
		root->addWidget(bb);
		d.resize(820, 480);
		d.exec();
	}

	/* ---- tabs ---- */

	QString AddTab()
	{
		bool ok = false;
		const QString name =
			QInputDialog::getText(this, "Новая вкладка", "Название", QLineEdit::Normal, QString(), &ok).trimmed();
		if (!ok || name.isEmpty() || name == "Все")
			return QString();
		for (const Tab &t : tabList_)
			if (t.name == name)
				return name;
		static const char *palette[] = {"#f28c1e", "#3a8ee6", "#3fb950", "#b56ce0", "#e0c13a", "#36c2c2"};
		tabList_.push_back({name, QColor(palette[tabList_.size() % 6])});
		SaveTabs();
		FillTabBar(name);
		return name;
	}

	void TabMenu(int index)
	{
		QMenu menu(this);
		QAction *add = menu.addAction("Новая вкладка…");
		QAction *rename = nullptr, *color = nullptr, *remove = nullptr;
		if (index > 0) {
			menu.addSeparator();
			rename = menu.addAction("Переименовать…");
			color = menu.addAction("Цвет…");
			remove = menu.addAction("Удалить вкладку (входы останутся)");
		}
		QAction *chosen = menu.exec(QCursor::pos());
		if (!chosen)
			return;
		if (chosen == add) {
			AddTab();
			return;
		}
		Tab &tab = tabList_[index - 1];
		const QString old = tab.name;
		QString select = old;
		if (chosen == rename) {
			bool ok = false;
			const QString name =
				QInputDialog::getText(this, "Переименовать вкладку", "Название", QLineEdit::Normal, old, &ok)
					.trimmed();
			if (!ok || name.isEmpty() || name == "Все")
				return;
			tab.name = name;
			for (Input &in : inputs_)
				if (in.tab == old)
					in.tab = name;
			select = name;
		} else if (chosen == color) {
			const QColor c = QColorDialog::getColor(tab.color, this, "Цвет вкладки");
			if (!c.isValid())
				return;
			tab.color = c;
		} else if (chosen == remove) {
			for (Input &in : inputs_)
				if (in.tab == old)
					in.tab.clear();
			tabList_.erase(tabList_.begin() + (index - 1));
			select.clear();
		}
		SaveTabs();
		SaveInputs();
		FillTabBar(select);
	}

	/* Fills the tab bar, selects `select` (empty = «Все») and rebuilds the tiles. */
	void FillTabBar(const QString &select)
	{
		restoringTabs_ = true;
		while (tabs_->count())
			tabs_->removeTab(0);
		tabs_->addTab("Все");
		int sel = 0;
		for (const Tab &t : tabList_) {
			const int i = tabs_->addTab(t.name);
			tabs_->setTabTextColor(i, t.color);
			if (t.name == select)
				sel = i;
		}
		tabs_->setCurrentIndex(sel);
		restoringTabs_ = false;
		QSettings().setValue("inputs/currentTab", CurrentTab());
		RebuildTiles();
	}

	void SaveTabs()
	{
		QJsonArray arr;
		for (const Tab &t : tabList_)
			arr.append(QJsonObject{{"name", t.name}, {"color", t.color.name()}});
		QSettings().setValue("inputs/tabs", QJsonDocument(arr).toJson(QJsonDocument::Compact));
	}

	void RestoreTabs()
	{
		const QJsonArray arr = QJsonDocument::fromJson(QSettings().value("inputs/tabs").toByteArray()).array();
		for (const auto &v : arr)
			tabList_.push_back({v.toObject()["name"].toString(), QColor(v.toObject()["color"].toString())});
		restoringTabs_ = true;
		tabs_->addTab("Все");
		const QString current = QSettings().value("inputs/currentTab").toString();
		for (const Tab &t : tabList_) {
			const int i = tabs_->addTab(t.name);
			tabs_->setTabTextColor(i, t.color);
			if (t.name == current)
				tabs_->setCurrentIndex(i);
		}
		restoringTabs_ = false;
	}

	/* ---- inputs ---- */

	void SaveInputs()
	{
		QJsonArray arr;
		const auto refs = InputRefs();
		for (int i = 0; i < (int)inputs_.size(); i++) {
			const Input &in = inputs_[i];
			OBSDataAutoRelease s = obs_source_get_settings(in.source);
			QJsonObject o;
			o["type"] = in.type;
			o["name"] = in.name;
			o["tab"] = in.tab;
			o["volume"] = (double)obs_source_get_volume(in.source);
			o["routes"] = (int)in.routes;
			o["triggers"] = triggersui::ToJson(sw_->Triggers(i), refs);
			o["devices"] = QJsonArray::fromStringList(in.devices);
			o["afv"] = in.afv;
			o["settings"] = QJsonDocument::fromJson(QByteArray(obs_data_get_json(s))).object();
			arr.append(o);
		}
		QSettings().setValue("inputs/list", QJsonDocument(arr).toJson(QJsonDocument::Compact));
	}

	void RestoreInputs()
	{
		const QJsonArray arr = QJsonDocument::fromJson(QSettings().value("inputs/list").toByteArray()).array();
		/* saved position -> input as restored (null if it could not be created): trigger targets
		 * are saved by position */
		std::vector<triggersui::InputRef> saved;
		std::vector<std::pair<int, QJsonArray>> triggers; /* (restored index, triggers) */
		for (const auto &v : arr) {
			const QJsonObject o = v.toObject();
			const QByteArray json = QJsonDocument(o["settings"].toObject()).toJson(QJsonDocument::Compact);
			OBSDataAutoRelease s = obs_data_create_from_json(json.constData());
			if (Add(o["type"].toString(), o["name"].toString(), s, o["tab"].toString(), o["volume"].toDouble(1.0),
				RoutesFrom(o), o["afv"].toBool(true), false, o["devices"].toVariant().toStringList())) {
				const int index = (int)inputs_.size() - 1;
				saved.push_back({o["name"].toString(), sw_->InputSource(index)});
				triggers.push_back({index, o["triggers"].toArray()});
			} else {
				saved.push_back({o["name"].toString(), nullptr});
			}
		}
		for (auto &[index, json] : triggers)
			sw_->SetTriggers(index, triggersui::FromJson(json, saved));
	}

	myobs::Switcher *sw_;
	std::vector<Input> inputs_;
	std::vector<Tab> tabList_;
	std::vector<InputTile *> tiles_;
	std::vector<int> tileInput_;
	QTabBar *tabs_ = nullptr;
	QWidget *host_ = nullptr;
	QGridLayout *grid_ = nullptr;
	QSpinBox *fadeMs_ = nullptr;
	QLabel *status_ = nullptr;
	int columns_ = 0;
	std::map<QString, std::shared_ptr<myobs::DeviceSink>> sinks_;
	std::unique_ptr<ScreenOutputs> screens_;
	myobs::Recorder recorder_;
	QPushButton *recordBtn_ = nullptr;
	qint64 recordStart_ = 0;
	bool restoringTabs_ = false;
};
