/* Generic properties dialog: built from the engine source's own properties (obs_properties), so
 * every input type (camera, DeckLink, NDI, ...) gets its settings without a dialog of its own.
 * Changes are applied to the source at once; Cancel restores the settings it had on opening. */
#pragma once

#include <obs.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

class PropertiesDialog : public QDialog {
public:
	PropertiesDialog(obs_source_t *source, const QString &title, QWidget *parent)
		: QDialog(parent), source_(source)
	{
		setWindowTitle(title);
		setMinimumWidth(560);
		original_ = obs_source_get_settings(source_);
		backup_ = obs_data_create();
		obs_data_apply(backup_, original_);

		auto *root = new QVBoxLayout(this);
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		host_ = new QWidget(scroll);
		scroll->setWidget(host_);
		root->addWidget(scroll, 1);
		auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
		root->addWidget(bb);
		Build();
		resize(620, 520);
	}

	~PropertiesDialog() override
	{
		if (props_)
			obs_properties_destroy(props_);
	}

	void reject() override
	{
		/* restore the settings the source had */
		OBSDataAutoRelease cur = obs_source_get_settings(source_);
		obs_data_clear(cur);
		obs_data_apply(cur, backup_);
		obs_source_update(source_, cur);
		QDialog::reject();
	}

private:
	void Build()
	{
		if (props_)
			obs_properties_destroy(props_);
		delete host_->layout();
		for (QObject *c : host_->children())
			if (c->isWidgetType())
				delete c;
		props_ = obs_source_properties(source_);
		auto *form = new QFormLayout(host_);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		if (!props_) {
			form->addRow(new QLabel("У этого входа нет настроек.", host_));
			return;
		}
		for (obs_property_t *p = obs_properties_first(props_); p; obs_property_next(&p))
			AddRow(form, p);
	}

	/* A property changed: apply to the source; rebuild if the property says the list changed. */
	void Changed(obs_property_t *p)
	{
		obs_source_update(source_, original_);
		if (obs_property_modified(p, original_))
			QTimer::singleShot(0, this, [this]() { Build(); });
	}

	void AddRow(QFormLayout *form, obs_property_t *p)
	{
		if (!obs_property_visible(p))
			return;
		const char *name = obs_property_name(p);
		const QString label = QString::fromUtf8(obs_property_description(p));
		QWidget *w = nullptr;
		switch (obs_property_get_type(p)) {
		case OBS_PROPERTY_BOOL: {
			auto *c = new QCheckBox(label, host_);
			c->setChecked(obs_data_get_bool(original_, name));
			connect(c, &QCheckBox::toggled, this, [this, p, name](bool v) {
				obs_data_set_bool(original_, name, v);
				Changed(p);
			});
			form->addRow("", c);
			c->setEnabled(obs_property_enabled(p));
			return;
		}
		case OBS_PROPERTY_INT: {
			auto *s = new QSpinBox(host_);
			s->setRange(obs_property_int_min(p), obs_property_int_max(p));
			s->setSingleStep(std::max(1, obs_property_int_step(p)));
			s->setSuffix(QString::fromUtf8(obs_property_int_suffix(p)));
			s->setValue((int)obs_data_get_int(original_, name));
			connect(s, &QSpinBox::valueChanged, this, [this, p, name](int v) {
				obs_data_set_int(original_, name, v);
				Changed(p);
			});
			w = s;
			break;
		}
		case OBS_PROPERTY_FLOAT: {
			auto *s = new QDoubleSpinBox(host_);
			s->setRange(obs_property_float_min(p), obs_property_float_max(p));
			s->setSingleStep(obs_property_float_step(p));
			s->setDecimals(3);
			s->setSuffix(QString::fromUtf8(obs_property_float_suffix(p)));
			s->setValue(obs_data_get_double(original_, name));
			connect(s, &QDoubleSpinBox::valueChanged, this, [this, p, name](double v) {
				obs_data_set_double(original_, name, v);
				Changed(p);
			});
			w = s;
			break;
		}
		case OBS_PROPERTY_TEXT: {
			if (obs_property_text_type(p) == OBS_TEXT_INFO) {
				auto *l = new QLabel(QString::fromUtf8(obs_data_get_string(original_, name)), host_);
				l->setWordWrap(true);
				form->addRow(label, l);
				return;
			}
			auto *e = new QLineEdit(QString::fromUtf8(obs_data_get_string(original_, name)), host_);
			if (obs_property_text_type(p) == OBS_TEXT_PASSWORD)
				e->setEchoMode(QLineEdit::Password);
			connect(e, &QLineEdit::editingFinished, this, [this, p, name, e]() {
				obs_data_set_string(original_, name, e->text().toUtf8().constData());
				Changed(p);
			});
			w = e;
			break;
		}
		case OBS_PROPERTY_PATH: {
			auto *row = new QWidget(host_);
			auto *h = new QHBoxLayout(row);
			h->setContentsMargins(0, 0, 0, 0);
			auto *e = new QLineEdit(QString::fromUtf8(obs_data_get_string(original_, name)), row);
			auto *b = new QPushButton("Обзор…", row);
			h->addWidget(e, 1);
			h->addWidget(b);
			const bool dir = obs_property_path_type(p) == OBS_PATH_DIRECTORY;
			const QString filter = QString::fromUtf8(obs_property_path_filter(p));
			auto apply = [this, p, name, e]() {
				obs_data_set_string(original_, name, e->text().toUtf8().constData());
				Changed(p);
			};
			connect(e, &QLineEdit::editingFinished, this, apply);
			connect(b, &QPushButton::clicked, this, [this, e, dir, filter, apply]() {
				const QString path = dir ? QFileDialog::getExistingDirectory(this, "Папка", e->text())
							 : QFileDialog::getOpenFileName(this, "Файл", e->text(), filter);
				if (!path.isEmpty()) {
					e->setText(path);
					apply();
				}
			});
			w = row;
			break;
		}
		case OBS_PROPERTY_LIST: {
			auto *c = new QComboBox(host_);
			const obs_combo_format fmt = obs_property_list_format(p);
			const size_t n = obs_property_list_item_count(p);
			int current = -1;
			for (size_t i = 0; i < n; i++) {
				const QString text = QString::fromUtf8(obs_property_list_item_name(p, i));
				if (fmt == OBS_COMBO_FORMAT_INT) {
					const long long v = obs_property_list_item_int(p, i);
					c->addItem(text, QVariant::fromValue(v));
					if (v == obs_data_get_int(original_, name))
						current = (int)i;
				} else if (fmt == OBS_COMBO_FORMAT_FLOAT) {
					const double v = obs_property_list_item_float(p, i);
					c->addItem(text, v);
					if (v == obs_data_get_double(original_, name))
						current = (int)i;
				} else {
					const QString v = QString::fromUtf8(obs_property_list_item_string(p, i));
					c->addItem(text, v);
					if (v == QString::fromUtf8(obs_data_get_string(original_, name)))
						current = (int)i;
				}
				if (obs_property_list_item_disabled(p, i))
					c->setItemData((int)i, 0, Qt::UserRole - 1);
			}
			c->setCurrentIndex(current);
			connect(c, &QComboBox::currentIndexChanged, this, [this, p, name, c, fmt](int) {
				const QVariant v = c->currentData();
				if (fmt == OBS_COMBO_FORMAT_INT)
					obs_data_set_int(original_, name, v.toLongLong());
				else if (fmt == OBS_COMBO_FORMAT_FLOAT)
					obs_data_set_double(original_, name, v.toDouble());
				else
					obs_data_set_string(original_, name, v.toString().toUtf8().constData());
				Changed(p);
			});
			w = c;
			break;
		}
		case OBS_PROPERTY_BUTTON: {
			auto *b = new QPushButton(label, host_);
			connect(b, &QPushButton::clicked, this, [this, p]() {
				if (obs_property_button_clicked(p, source_))
					QTimer::singleShot(0, this, [this]() { Build(); });
			});
			form->addRow("", b);
			b->setEnabled(obs_property_enabled(p));
			return;
		}
		case OBS_PROPERTY_GROUP: {
			auto *title = new QLabel("<b>" + label.toHtmlEscaped() + "</b>", host_);
			form->addRow(title);
			obs_properties_t *group = obs_property_group_content(p);
			for (obs_property_t *q = obs_properties_first(group); q; obs_property_next(&q))
				AddRow(form, q);
			return;
		}
		default:
			return; /* colours, fonts, editable lists: added when an input needs them */
		}
		w->setEnabled(obs_property_enabled(p));
		const QString tip = QString::fromUtf8(obs_property_long_description(p));
		if (!tip.isEmpty())
			w->setToolTip(tip);
		form->addRow(label, w);
	}

	obs_source_t *source_;
	OBSDataAutoRelease original_;
	OBSDataAutoRelease backup_;
	obs_properties_t *props_ = nullptr;
	QWidget *host_ = nullptr;
};
