/* Trigger editor of an input (right button on a tile → «Триггеры…»), after the concept sketch:
 * two sections - «Когда уходит в эфир», «Когда уходит из эфира» - with rows of actions:
 *   [Действие] [Цель] [Значение / экран / переход] [из тишины] [за, с] [через, с] [×]
 * Also JSON (de)serialisation; targets are saved by input position. */
#pragma once

#include <obs.hpp>

#include "triggers.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include <functional>
#include <vector>

namespace triggersui {

struct InputRef {
	QString name;
	obs_source_t *source;
};

inline int IndexOf(const std::vector<InputRef> &inputs, obs_source_t *s)
{
	for (size_t i = 0; i < inputs.size(); i++)
		if (inputs[i].source == s)
			return (int)i;
	return -1;
}

inline QJsonArray ToJson(const std::vector<myobs::Trigger> &triggers, const std::vector<InputRef> &inputs)
{
	QJsonArray arr;
	for (const myobs::Trigger &t : triggers) {
		QJsonArray actions;
		for (const myobs::TriggerAction &a : t.actions) {
			QJsonObject o;
			o["kind"] = (int)a.kind;
			o["target"] = a.target ? IndexOf(inputs, a.target) : -1;
			o["level"] = (int)a.level;
			o["db"] = a.db;
			o["fromSilence"] = a.fromSilence;
			o["ramp"] = a.rampSec;
			o["delay"] = a.delaySec;
			o["transitionMs"] = (int)a.transitionMs;
			o["screen"] = QString::fromStdString(a.screen);
			actions.append(o);
		}
		arr.append(QJsonObject{{"event", (int)t.event}, {"actions", actions}});
	}
	return arr;
}

inline std::vector<myobs::Trigger> FromJson(const QJsonArray &arr, const std::vector<InputRef> &inputs)
{
	std::vector<myobs::Trigger> out;
	for (const auto &v : arr) {
		const QJsonObject t = v.toObject();
		myobs::Trigger trig;
		trig.event = (myobs::TriggerEvent)t["event"].toInt();
		for (const auto &av : t["actions"].toArray()) {
			const QJsonObject o = av.toObject();
			myobs::TriggerAction a;
			a.kind = (myobs::TriggerAction::Kind)o["kind"].toInt();
			const int target = o["target"].toInt(-1);
			a.target = target >= 0 && target < (int)inputs.size() ? inputs[target].source : nullptr;
			if (target >= 0 && !a.target)
				continue; /* the target input is gone */
			a.level = (myobs::TriggerAction::Level)o["level"].toInt();
			a.db = o["db"].toDouble();
			a.fromSilence = o["fromSilence"].toBool();
			a.rampSec = o["ramp"].toDouble(1.0);
			a.delaySec = o["delay"].toDouble(0.0);
			a.transitionMs = (uint32_t)o["transitionMs"].toInt(0);
			a.screen = o["screen"].toString().toStdString();
			trig.actions.push_back(a);
		}
		out.push_back(trig);
	}
	return out;
}

/* Edits `triggers` in place; returns true on OK. screens: (id, title). */
inline bool Edit(QWidget *parent, const QString &inputName, std::vector<myobs::Trigger> &triggers,
		 const std::vector<InputRef> &inputs, const std::vector<std::pair<QString, QString>> &screens)
{
	using A = myobs::TriggerAction;
	/* working copy: one list per event */
	std::vector<A> lists[2];
	for (const myobs::Trigger &t : triggers)
		for (const A &a : t.actions)
			lists[(int)t.event].push_back(a);

	QDialog d(parent);
	d.setWindowTitle(QString("Триггеры — %1").arg(inputName));
	auto *root = new QVBoxLayout(&d);
	auto *scroll = new QScrollArea(&d);
	scroll->setWidgetResizable(true);
	auto *host = new QWidget(scroll);
	scroll->setWidget(host);
	root->addWidget(scroll, 1);
	auto *grid = new QGridLayout(host);
	grid->setHorizontalSpacing(6);

	std::function<void()> rebuild;
	rebuild = [&]() {
		while (QLayoutItem *item = grid->takeAt(0)) {
			if (item->widget())
				item->widget()->deleteLater();
			delete item;
		}
		int row = 0;
		static const char *titles[2] = {"Когда уходит в эфир", "Когда уходит из эфира"};
		for (int ev = 0; ev < 2; ev++) {
			auto *title = new QLabel(QString("<b>%1</b>").arg(QString::fromUtf8(titles[ev])), host);
			grid->addWidget(title, row, 0, 1, 4);
			auto *add = new QPushButton("+ Действие", host);
			grid->addWidget(add, row, 6, 1, 2);
			QObject::connect(add, &QPushButton::clicked, &d, [&, ev]() {
				lists[ev].push_back(A{});
				QMetaObject::invokeMethod(&d, [&]() { rebuild(); }, Qt::QueuedConnection);
			});
			row++;
			for (size_t k = 0; k < lists[ev].size(); k++) {
				A *a = &lists[ev][k];
				auto *kind = new QComboBox(host);
				kind->addItem("Звук: громкость", (int)A::Kind::Volume);
				kind->addItem("Перейти в эфир", (int)A::Kind::GoOnAir);
				kind->addItem("Экран: показать", (int)A::Kind::ShowScreen);
				kind->setCurrentIndex(kind->findData((int)a->kind));
				auto *target = new QComboBox(host);
				target->addItem("этот вход", -1);
				for (size_t i = 0; i < inputs.size(); i++)
					target->addItem(inputs[i].name, (int)i);
				target->setCurrentIndex(std::max(0, target->findData(a->target ? IndexOf(inputs, a->target) : -1)));
				grid->addWidget(kind, row, 0);
				grid->addWidget(target, row, 1);
				QObject::connect(kind, &QComboBox::currentIndexChanged, &d, [&, a, kind]() {
					a->kind = (A::Kind)kind->currentData().toInt();
					QMetaObject::invokeMethod(&d, [&]() { rebuild(); }, Qt::QueuedConnection);
				});
				QObject::connect(target, &QComboBox::currentIndexChanged, &d, [a, target, &inputs]() {
					const int i = target->currentData().toInt();
					a->target = i >= 0 ? inputs[i].source : nullptr;
				});
				if (a->kind == A::Kind::Volume) {
					auto *level = new QComboBox(host);
					level->addItem("выкл (−∞)", (int)A::Level::Off);
					level->addItem("как было", (int)A::Level::Restore);
					level->addItem("уровень, дБ", (int)A::Level::Db);
					level->setCurrentIndex(level->findData((int)a->level));
					auto *db = new QDoubleSpinBox(host);
					db->setRange(-60.0, 12.0);
					db->setSuffix(" дБ");
					db->setValue(a->db);
					db->setEnabled(a->level == A::Level::Db);
					auto *fromSilence = new QCheckBox("из тишины", host);
					fromSilence->setChecked(a->fromSilence);
					fromSilence->setToolTip("Начать с тишины: вход плавно выходит");
					auto *ramp = new QDoubleSpinBox(host);
					ramp->setRange(0.0, 60.0);
					ramp->setSingleStep(0.5);
					ramp->setPrefix("за ");
					ramp->setSuffix(" с");
					ramp->setValue(a->rampSec);
					grid->addWidget(level, row, 2);
					grid->addWidget(db, row, 3);
					grid->addWidget(fromSilence, row, 4);
					grid->addWidget(ramp, row, 5);
					QObject::connect(level, &QComboBox::currentIndexChanged, &d, [a, level, db]() {
						a->level = (A::Level)level->currentData().toInt();
						db->setEnabled(a->level == A::Level::Db);
					});
					QObject::connect(db, &QDoubleSpinBox::valueChanged, &d, [a](double v) { a->db = v; });
					QObject::connect(fromSilence, &QCheckBox::toggled, &d, [a](bool v) { a->fromSilence = v; });
					QObject::connect(ramp, &QDoubleSpinBox::valueChanged, &d, [a](double v) { a->rampSec = v; });
				} else if (a->kind == A::Kind::GoOnAir) {
					auto *tr = new QSpinBox(host);
					tr->setRange(0, 10000);
					tr->setSingleStep(100);
					tr->setSpecialValueText("CUT");
					tr->setPrefix("FADE ");
					tr->setSuffix(" мс");
					tr->setValue((int)a->transitionMs);
					grid->addWidget(tr, row, 2, 1, 2);
					QObject::connect(tr, &QSpinBox::valueChanged, &d, [a](int v) { a->transitionMs = (uint32_t)v; });
				} else {
					auto *screen = new QComboBox(host);
					for (auto &[id, name] : screens)
						screen->addItem(name, id);
					screen->setCurrentIndex(std::max(0, screen->findData(QString::fromStdString(a->screen))));
					if (a->screen.empty() && screen->count())
						a->screen = screen->currentData().toString().toStdString();
					grid->addWidget(screen, row, 2, 1, 2);
					QObject::connect(screen, &QComboBox::currentIndexChanged, &d, [a, screen]() {
						a->screen = screen->currentData().toString().toStdString();
					});
				}
				auto *delay = new QDoubleSpinBox(host);
				delay->setRange(0.0, 3600.0);
				delay->setSingleStep(0.5);
				delay->setPrefix("через ");
				delay->setSuffix(" с");
				delay->setValue(a->delaySec);
				grid->addWidget(delay, row, 6);
				QObject::connect(delay, &QDoubleSpinBox::valueChanged, &d, [a](double v) { a->delaySec = v; });
				auto *del = new QPushButton("×", host);
				del->setFixedWidth(26);
				grid->addWidget(del, row, 7);
				QObject::connect(del, &QPushButton::clicked, &d, [&, ev, k]() {
					lists[ev].erase(lists[ev].begin() + (long)k);
					QMetaObject::invokeMethod(&d, [&]() { rebuild(); }, Qt::QueuedConnection);
				});
				row++;
			}
			row++;
		}
		grid->setRowStretch(row, 1);
	};
	rebuild();

	auto *hint = new QLabel("«как было» возвращает громкость к фейдеру входа; фейдер триггеры не двигают.", &d);
	hint->setWordWrap(true);
	root->addWidget(hint);
	auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
	QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
	root->addWidget(bb);
	d.resize(900, 460);
	if (d.exec() != QDialog::Accepted)
		return false;
	triggers.clear();
	for (int ev = 0; ev < 2; ev++)
		if (!lists[ev].empty())
			triggers.push_back({(myobs::TriggerEvent)ev, lists[ev]});
	return true;
}

} // namespace triggersui
