/* MyOBS inputs panel.
 *
 * Tiles: live thumbnail, number, type, name; audio meter, fader and mute for inputs with sound.
 * Click = Preview, double click = on air by CUT, right button = properties, rename, tab, remove.
 * Tabs: «Все» plus own colour-coded tabs; an input belongs to one tab or none.
 * CUT / FADE with the fade length; + Клип / + Картинка / + Цвет.
 * Inputs, their volume and mute, and the tabs are saved and restored between starts. */
#pragma once

#include <obs.hpp>

#include "display.hpp"
#include "props.hpp"
#include "switcher.hpp"
#include "thumbs.hpp"

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
#include <memory>
#include <vector>

/* ---- audio of one input: meter (engine audio thread -> atomic) and fader ---- */

class InputAudio {
public:
	explicit InputAudio(obs_source_t *source)
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
	float TakePeakDb() { return peak_.exchange(-100.0f); }
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
			mute_ = new QPushButton("M", this);
			mute_->setObjectName("mute");
			mute_->setCheckable(true);
			mute_->setFixedSize(26, 20);
			mute_->setToolTip("Выключить звук входа");
			row->addWidget(fader_, 1);
			row->addWidget(mute_);
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
	QPushButton *Mute() const { return mute_; }
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
	QPushButton *mute_ = nullptr;
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
		connect(addClip, &QPushButton::clicked, this, [this]() { AddClipDialog(); });
		connect(addImage, &QPushButton::clicked, this, [this]() { AddImageDialog(); });
		connect(addColor, &QPushButton::clicked, this, [this]() { AddColorDialog(); });
		bar->addWidget(addClip);
		bar->addWidget(addImage);
		bar->addWidget(addColor);
		bar->addWidget(addCamera);
		bar->addWidget(addDeckLink);
		bar->addWidget(addNdi);
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
		RebuildTiles();
	}

	~InputsPanel() override
	{
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
					    : type;
	}

	static const char *SourceId(const QString &type)
	{
		return type == "clip"       ? "myobs_clip"
		       : type == "image"    ? "image_source"
		       : type == "camera"   ? "dshow_input"
		       : type == "decklink" ? "decklink-input"
		       : type == "ndi"      ? "myobs_ndi"
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
		 bool muted, bool save = true)
	{
		OBSSourceAutoRelease src = obs_source_create(SourceId(type), name.toUtf8().constData(), settings, nullptr);
		if (!src) {
			QMessageBox::warning(this, "MyOBS", QString("Не удалось создать вход «%1».").arg(name));
			return false;
		}
		obs_source_set_volume(src, (float)volume);
		obs_source_set_muted(src, muted);
		const bool hasAudio = (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0;
		sw_->AddInput(src, name.toStdString());
		Input in{type, name, tab, std::move(src), nullptr};
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
			Add(type, QFileInfo(f).completeBaseName(), s, CurrentTab(), 1.0, false);
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
		Add("color", QString("Цвет %1").arg(c.name()), s, CurrentTab(), 1.0, false);
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
		if (!Add(type, QString("%1 %2").arg(base).arg(n), s, CurrentTab(), 1.0, false))
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
		const bool hasAudio = (obs_source_get_output_flags(in.source) & OBS_SOURCE_AUDIO) != 0;
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
				t->Mute()->setChecked(obs_source_muted(in.source));
				obs_source_t *src = in.source;
				InputAudio *audio = in.audio.get();
				connect(t->Fader(), &QSlider::valueChanged, this, [audio](int v) { audio->SetDeflection(v / 1000.0f); });
				connect(t->Fader(), &QSlider::sliderReleased, this, [this]() { SaveInputs(); });
				connect(t->Mute(), &QPushButton::toggled, this, [this, src](bool m) {
					obs_source_set_muted(src, m);
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
		RebuildTiles();
		SaveInputs();
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
		for (const Input &in : inputs_) {
			OBSDataAutoRelease s = obs_source_get_settings(in.source);
			QJsonObject o;
			o["type"] = in.type;
			o["name"] = in.name;
			o["tab"] = in.tab;
			o["volume"] = (double)obs_source_get_volume(in.source);
			o["muted"] = obs_source_muted(in.source);
			o["settings"] = QJsonDocument::fromJson(QByteArray(obs_data_get_json(s))).object();
			arr.append(o);
		}
		QSettings().setValue("inputs/list", QJsonDocument(arr).toJson(QJsonDocument::Compact));
	}

	void RestoreInputs()
	{
		const QJsonArray arr = QJsonDocument::fromJson(QSettings().value("inputs/list").toByteArray()).array();
		for (const auto &v : arr) {
			const QJsonObject o = v.toObject();
			const QByteArray json = QJsonDocument(o["settings"].toObject()).toJson(QJsonDocument::Compact);
			OBSDataAutoRelease s = obs_data_create_from_json(json.constData());
			Add(o["type"].toString(), o["name"].toString(), s, o["tab"].toString(), o["volume"].toDouble(1.0),
			    o["muted"].toBool(false), false);
		}
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
	bool restoringTabs_ = false;
};
