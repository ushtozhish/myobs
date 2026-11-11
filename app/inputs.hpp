/* MyOBS inputs panel: tiles (click = Preview, double click = on air by CUT), CUT / FADE, adding clips,
 * images and colours, properties, removal. The list of inputs is saved and restored between starts. */
#pragma once

#include <obs.hpp>

#include "switcher.hpp"

#include <QAction>
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <vector>

class InputTile : public QFrame {
public:
	InputTile(int index, QWidget *parent) : QFrame(parent), index_(index)
	{
		setObjectName("tile");
		setFixedSize(176, 64);
		setCursor(Qt::PointingHandCursor);
		auto *l = new QVBoxLayout(this);
		l->setContentsMargins(8, 5, 8, 5);
		l->setSpacing(1);
		number_ = new QLabel(this);
		number_->setObjectName("tileNumber");
		name_ = new QLabel(this);
		name_->setObjectName("tileName");
		l->addWidget(number_);
		l->addWidget(name_);
		l->addStretch();
		setToolTip("Клик — в Preview\nДвойной клик — сразу в эфир (CUT)\nПравая кнопка — свойства, удалить");
	}

	void SetText(const QString &type, const QString &name)
	{
		number_->setText(QString("%1  ·  %2").arg(index_ + 1).arg(type));
		name_->setText(fontMetrics().elidedText(name, Qt::ElideMiddle, 156));
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
	}

	std::function<void(int)> onClick, onDoubleClick, onMenu;

protected:
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
	QLabel *number_ = nullptr;
	QLabel *name_ = nullptr;
	const char *state_ = "";
};

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
		connect(addClip, &QPushButton::clicked, this, [this]() { AddClipDialog(); });
		connect(addImage, &QPushButton::clicked, this, [this]() { AddImageDialog(); });
		connect(addColor, &QPushButton::clicked, this, [this]() { AddColorDialog(); });
		bar->addWidget(addClip);
		bar->addWidget(addImage);
		bar->addWidget(addColor);
		bar->addStretch();
		status_ = new QLabel(this);
		status_->setObjectName("switchStatus");
		bar->addWidget(status_);
		root->addLayout(bar);

		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		auto *host = new QWidget(scroll);
		host->setObjectName("tilesHost");
		grid_ = new QGridLayout(host);
		grid_->setContentsMargins(0, 0, 0, 0);
		grid_->setSpacing(6);
		grid_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
		scroll->setWidget(host);
		root->addWidget(scroll, 1);

		Restore();
	}

	/* Called by the window timer: tile colours and the switch status. */
	void Refresh()
	{
		const int program = sw_->Program(), preview = sw_->Preview();
		for (int i = 0; i < (int)tiles_.size(); i++)
			tiles_[i]->SetState(i == program, i == preview);
		const size_t pending = sw_->PendingRequests();
		status_->setText(sw_->Transitioning() ? "переход…" : pending ? QString("в очереди: %1").arg(pending) : "");
	}

private:
	struct Input {
		QString type; // clip | image | color
		QString name;
		OBSSourceAutoRelease source;
	};

	static QString TypeTitle(const QString &type)
	{
		return type == "clip" ? "Клип" : type == "image" ? "Картинка" : type == "color" ? "Цвет" : type;
	}

	static const char *SourceId(const QString &type)
	{
		return type == "clip" ? "myobs_clip" : type == "image" ? "image_source" : "color_source_v3";
	}

	bool Add(const QString &type, const QString &name, obs_data_t *settings, bool save = true)
	{
		OBSSourceAutoRelease src =
			obs_source_create(SourceId(type), name.toUtf8().constData(), settings, nullptr);
		if (!src) {
			QMessageBox::warning(this, "MyOBS", QString("Не удалось создать вход «%1».").arg(name));
			return false;
		}
		sw_->AddInput(src, name.toStdString());
		inputs_.push_back({type, name, std::move(src)});
		RebuildTiles();
		if (save)
			Save();
		return true;
	}

	void AddClipDialog()
	{
		const QStringList files = QFileDialog::getOpenFileNames(
			this, "Клипы", QSettings().value("paths/clips").toString(),
			"Видео (*.mp4 *.mov *.mkv *.avi *.mxf *.m4v *.ts *.mts *.webm);;Все файлы (*.*)");
		for (const QString &f : files) {
			QSettings().setValue("paths/clips", QFileInfo(f).absolutePath());
			OBSDataAutoRelease s = obs_data_create();
			obs_data_set_string(s, "file", f.toUtf8().constData());
			Add("clip", QFileInfo(f).completeBaseName(), s);
		}
	}

	void AddImageDialog()
	{
		const QStringList files = QFileDialog::getOpenFileNames(
			this, "Картинки", QSettings().value("paths/images").toString(),
			"Картинки (*.png *.jpg *.jpeg *.bmp *.tga *.gif *.webp);;Все файлы (*.*)");
		for (const QString &f : files) {
			QSettings().setValue("paths/images", QFileInfo(f).absolutePath());
			OBSDataAutoRelease s = obs_data_create();
			obs_data_set_string(s, "file", f.toUtf8().constData());
			Add("image", QFileInfo(f).completeBaseName(), s);
		}
	}

	void AddColorDialog()
	{
		const QColor c = QColorDialog::getColor(Qt::black, this, "Цвет");
		if (!c.isValid())
			return;
		OBSDataAutoRelease s = obs_data_create();
		SetColor(s, c);
		Add("color", QString("Цвет %1").arg(c.name()), s);
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

	void RebuildTiles()
	{
		for (InputTile *t : tiles_)
			t->deleteLater();
		tiles_.clear();
		const int columns = 6;
		for (int i = 0; i < (int)inputs_.size(); i++) {
			auto *t = new InputTile(i, this);
			t->SetText(TypeTitle(inputs_[i].type), inputs_[i].name);
			t->onClick = [this](int idx) { sw_->SetPreview(idx); };
			t->onDoubleClick = [this](int idx) {
				sw_->SetPreview(idx);
				sw_->Cut();
			};
			t->onMenu = [this](int idx) { Menu(idx); };
			grid_->addWidget(t, i / columns, i % columns);
			tiles_.push_back(t);
		}
		Refresh();
	}

	void Menu(int index)
	{
		if (index < 0 || index >= (int)inputs_.size())
			return;
		QMenu menu(this);
		QAction *props = menu.addAction("Свойства…");
		QAction *rename = menu.addAction("Переименовать…");
		menu.addSeparator();
		QAction *remove = menu.addAction("Удалить");
		QAction *chosen = menu.exec(QCursor::pos());
		if (chosen == props)
			Properties(index);
		else if (chosen == rename)
			Rename(index);
		else if (chosen == remove)
			Remove(index);
	}

	void Rename(int index)
	{
		QDialog d(this);
		d.setWindowTitle("Переименовать");
		auto *f = new QFormLayout(&d);
		auto *edit = new QLineEdit(inputs_[index].name, &d);
		f->addRow("Название", edit);
		auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
		f->addRow(bb);
		connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
		connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
		if (d.exec() != QDialog::Accepted || edit->text().trimmed().isEmpty())
			return;
		inputs_[index].name = edit->text().trimmed();
		obs_source_set_name(inputs_[index].source, inputs_[index].name.toUtf8().constData());
		RebuildTiles();
		Save();
	}

	void Properties(int index)
	{
		Input &in = inputs_[index];
		OBSDataAutoRelease s = obs_source_get_settings(in.source);
		if (in.type == "color") {
			const long long abgr = obs_data_get_int(s, "color");
			const QColor c = QColorDialog::getColor(QColor(abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF),
								this, "Цвет");
			if (!c.isValid())
				return;
			SetColor(s, c);
			obs_source_update(in.source, s);
			Save();
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
			offair->setCurrentIndex(offair->findData((int)obs_data_get_int(s, "offair")));
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
		Save();
	}

	void Remove(int index)
	{
		if (!sw_->RemoveInput(index)) {
			QMessageBox::information(this, "MyOBS", "Вход сейчас в эфире или в переходе — его нельзя удалить.");
			return;
		}
		inputs_.erase(inputs_.begin() + index);
		RebuildTiles();
		Save();
	}

	void Save()
	{
		QJsonArray arr;
		for (const Input &in : inputs_) {
			OBSDataAutoRelease s = obs_source_get_settings(in.source);
			QJsonObject o;
			o["type"] = in.type;
			o["name"] = in.name;
			o["settings"] = QJsonDocument::fromJson(QByteArray(obs_data_get_json(s))).object();
			arr.append(o);
		}
		QSettings().setValue("inputs/list", QJsonDocument(arr).toJson(QJsonDocument::Compact));
	}

	void Restore()
	{
		const QJsonArray arr = QJsonDocument::fromJson(QSettings().value("inputs/list").toByteArray()).array();
		for (const auto &v : arr) {
			const QJsonObject o = v.toObject();
			const QByteArray json = QJsonDocument(o["settings"].toObject()).toJson(QJsonDocument::Compact);
			OBSDataAutoRelease s = obs_data_create_from_json(json.constData());
			Add(o["type"].toString(), o["name"].toString(), s, false);
		}
	}

	myobs::Switcher *sw_;
	std::vector<Input> inputs_;
	std::vector<InputTile *> tiles_;
	QGridLayout *grid_ = nullptr;
	QSpinBox *fadeMs_ = nullptr;
	QLabel *status_ = nullptr;
};
