#include "camera/FeaturePanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVector>
#include <QStringList>
#include <QEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using namespace VmbCPP;

namespace {

class WheelBlocker : public QObject {
public:
	explicit WheelBlocker(QObject* parent = nullptr)
		: QObject(parent) {}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override {
		if (event->type() == QEvent::Wheel) {
			event->ignore();
			return true;
		}
		return QObject::eventFilter(watched, event);
	}
};

QString displayName(const FeaturePtr& feature) {
	std::string name;
	if (VmbErrorSuccess == feature->GetName(name) && !name.empty()) {
		return QString::fromStdString(name);
	}
	return QStringLiteral("(未知)");
}

QString description(const FeaturePtr& feature) {
	std::string desc;
	if (VmbErrorSuccess == feature->GetDescription(desc) && !desc.empty()) {
		return QString::fromStdString(desc);
	}
	return {};
}

QString categoryKey(const FeaturePtr& feature) {
	if (!feature) {
		return QStringLiteral("Misc");
	}
	std::string category;
	if (feature->GetCategory(category) == VmbErrorSuccess && !category.empty()) {
		QString qCategory = QString::fromStdString(category);
		const QStringList parts = qCategory.split('/', Qt::SkipEmptyParts);
		if (!parts.isEmpty()) {
			return parts.first().trimmed();
		}
		return qCategory.trimmed();
	}
	return QStringLiteral("Misc");
}

QString categoryDisplay(const QString& key) {
	const QMap<QString, QString> mapping{
		{QStringLiteral("AcquisitionControl"), FeaturePanel::tr("采集控制")},
		{QStringLiteral("ImageFormatControl"), FeaturePanel::tr("图像格式")},
		{QStringLiteral("AnalogControl"), FeaturePanel::tr("模拟调整")},
		{QStringLiteral("Exposure"), FeaturePanel::tr("曝光控制")},
		{QStringLiteral("DigitalIOControl"), FeaturePanel::tr("IO 控制")},
		{QStringLiteral("DeviceControl"), FeaturePanel::tr("设备管理")},
		{QStringLiteral("Stream"), FeaturePanel::tr("数据流控制")},
		{QStringLiteral("ChunkDataControl"), FeaturePanel::tr("数据块")},
		{QStringLiteral("Misc"), FeaturePanel::tr("其他")}
	};
	if (key.isEmpty()) {
		return FeaturePanel::tr("其他");
	}
	return mapping.value(key, key);
}

int categoryPriority(const QString& key) {
	const QStringList order{
		QStringLiteral("AcquisitionControl"),
		QStringLiteral("ImageFormatControl"),
		QStringLiteral("Exposure"),
		QStringLiteral("AnalogControl"),
		QStringLiteral("Stream"),
		QStringLiteral("DigitalIOControl"),
		QStringLiteral("DeviceControl"),
		QStringLiteral("ChunkDataControl"),
		QStringLiteral("Misc")
	};
	const int idx = order.indexOf(key);
	return idx >= 0 ? idx : order.size() + 1;
}

} // namespace


FeaturePanel::FeaturePanel(CameraPtr cam, QWidget* parent)
	: QWidget(parent)
	, m_cam(std::move(cam)) {
	auto* outer = new QVBoxLayout(this);
	outer->setContentsMargins(16, 16, 16, 16);
	outer->setSpacing(10);

	m_messageLabel = new QLabel(this);
	m_messageLabel->setAlignment(Qt::AlignCenter);
	m_messageLabel->setWordWrap(true);
	m_messageLabel->setStyleSheet(QStringLiteral("color: rgba(210, 223, 245, 0.72); font-size: 13px;"));
	outer->addWidget(m_messageLabel);

	m_tabWidget = new QTabWidget(this);
	m_tabWidget->setObjectName(QStringLiteral("FeatureTabs"));
	m_tabWidget->setDocumentMode(true);
	m_tabWidget->setTabPosition(QTabWidget::North);
	m_tabWidget->setTabShape(QTabWidget::Rounded);
	m_tabWidget->setUsesScrollButtons(true);
	m_tabWidget->setElideMode(Qt::ElideRight);
	m_tabWidget->setVisible(false);
	outer->addWidget(m_tabWidget, 1);

	refresh();
}


void FeaturePanel::setCamera(CameraPtr cam) {
	m_cam = std::move(cam);
	refresh();
}


void FeaturePanel::refresh() {
	if (!m_tabWidget || !m_messageLabel) {
		return;
	}

	m_tabWidget->clear();

	auto showMessage = [this](const QString& text) {
		m_messageLabel->setText(text);
		m_messageLabel->setVisible(true);
		m_tabWidget->setVisible(false);
	};

	if (!m_cam) {
		showMessage(tr("未连接相机"));
		return;
	}

	FeaturePtrVector rawFeatures;
	if (m_cam->GetFeatures(rawFeatures) != VmbErrorSuccess) {
		showMessage(tr("无法获取特性列表"));
		return;
	}

	std::vector<FeaturePtr> features(rawFeatures.begin(), rawFeatures.end());
	std::sort(features.begin(), features.end(), [](const FeaturePtr& a, const FeaturePtr& b) {
		std::string na;
		std::string nb;
		a->GetName(na);
		b->GetName(nb);
		return QString::fromStdString(na).compare(QString::fromStdString(nb), Qt::CaseInsensitive) < 0;
	});

	QMap<QString, QVector<FeaturePtr>> categorized;
	for (const FeaturePtr& feature : features) {
		categorized[categoryKey(feature)].append(feature);
	}

	QStringList categories = categorized.keys();
	std::sort(categories.begin(), categories.end(), [](const QString& a, const QString& b) {
		const int pa = categoryPriority(a);
		const int pb = categoryPriority(b);
		if (pa == pb) {
			return QString::localeAwareCompare(a, b) < 0;
		}
		return pa < pb;
	});

	int tabsAdded = 0;
	for (const QString& category : categories) {
		const QVector<FeaturePtr> entries = categorized.value(category);

		auto* page = new QWidget;
		auto* pageLayout = new QVBoxLayout(page);
		pageLayout->setContentsMargins(0, 0, 0, 0);
		pageLayout->setSpacing(12);

		auto* sectionFrame = new QFrame(page);
		sectionFrame->setObjectName(QStringLiteral("FeatureSection"));
		auto* sectionLayout = new QVBoxLayout(sectionFrame);
		sectionLayout->setContentsMargins(0, 0, 0, 0);
		sectionLayout->setSpacing(10);

		auto* header = new QLabel(categoryDisplay(category));
		header->setObjectName(QStringLiteral("FeatureSectionHeader"));
		sectionLayout->addWidget(header);

		auto* grid = new QGridLayout();
		grid->setColumnStretch(0, 0);
		grid->setColumnStretch(1, 1);
		grid->setHorizontalSpacing(12);
		grid->setVerticalSpacing(10);

		int row = 0;
		for (const FeaturePtr& feature : entries) {
			QWidget* editor = makeEditor(feature);
			if (!editor) {
				continue;
			}

			auto* label = new QLabel(displayName(feature));
			QString tip = description(feature);
			if (!tip.isEmpty()) {
				label->setToolTip(tip);
				editor->setToolTip(tip);
			}

			grid->addWidget(label, row, 0);
			grid->addWidget(editor, row, 1);
			++row;
		}

		if (row == 0) {
			delete grid;
			delete sectionFrame;
			delete page;
			continue;
		}

		sectionLayout->addLayout(grid);
		pageLayout->addWidget(sectionFrame);
		pageLayout->addStretch(1);

		auto* scroll = new QScrollArea(m_tabWidget);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		scroll->setWidget(page);

		const QString tabLabel = categoryDisplay(category);
		m_tabWidget->addTab(scroll, tabLabel);
		m_tabWidget->setTabToolTip(m_tabWidget->count() - 1, tabLabel);
		++tabsAdded;
	}

	if (tabsAdded == 0) {
		showMessage(tr("没有可显示的特性"));
		return;
	}

	m_messageLabel->setVisible(false);
	m_tabWidget->setVisible(true);
	m_tabWidget->setCurrentIndex(0);
}


QWidget* FeaturePanel::makeEditor(const FeaturePtr& feature) {
	if (!feature) {
		return nullptr;
	}

	VmbFeatureDataType type;
	if (feature->GetDataType(type) != VmbErrorSuccess) {
		return nullptr;
	}

	bool readable = false;
	feature->IsReadable(readable);
	if (!readable && type != VmbFeatureDataCommand) {
		return nullptr;
	}

	bool writable = false;
	feature->IsWritable(writable);

	switch (type) {
	case VmbFeatureDataInt: {
		VmbInt64_t min = 0;
		VmbInt64_t max = 0;
		VmbInt64_t value = 0;
		feature->GetRange(min, max);
		feature->GetValue(value);
		auto* widget = new QSpinBox;
		const auto lo = static_cast<int>(std::clamp(
			min,
			static_cast<VmbInt64_t>((std::numeric_limits<int>::lowest)()),
			static_cast<VmbInt64_t>((std::numeric_limits<int>::max)())));
		const auto hi = static_cast<int>(std::clamp(
			max,
			static_cast<VmbInt64_t>((std::numeric_limits<int>::lowest)()),
			static_cast<VmbInt64_t>((std::numeric_limits<int>::max)())));
		widget->setRange(lo, hi);
		widget->setValue(static_cast<int>(std::clamp(value,
			static_cast<VmbInt64_t>(lo), static_cast<VmbInt64_t>(hi))));
		widget->setEnabled(writable);
		configureEditorWidget(widget);
		QObject::connect(widget, QOverload<int>::of(&QSpinBox::valueChanged), [feature](int v) {
			feature->SetValue(static_cast<VmbInt64_t>(v));
		});
		return widget;
	}
	case VmbFeatureDataFloat: {
		double min = 0.0;
		double max = 0.0;
		double value = 0.0;
		feature->GetRange(min, max);
		feature->GetValue(value);
		auto* widget = new QDoubleSpinBox;
		widget->setDecimals(6);
		double step = (max > min) ? ((max - min) / 100.0) : 0.0;
		if (!std::isfinite(step) || step <= 0.0) {
			step = 0.1;
		}
		widget->setSingleStep(step);
		widget->setRange(min, max);
		widget->setValue(value);
		widget->setEnabled(writable);
		configureEditorWidget(widget);
		QObject::connect(widget, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [feature](double v) {
			feature->SetValue(v);
		});
		return widget;
	}
	case VmbFeatureDataEnum: {
		EnumEntryVector entries;
		feature->GetEntries(entries);
		auto* widget = new QComboBox;
		widget->setEnabled(writable);
		std::string current;
		feature->GetValue(current);
		int idx = 0;
		int currentIdx = -1;
		for (const auto& entry : entries) {
			std::string symbolic;
			entry.GetName(symbolic);
			widget->addItem(QString::fromLocal8Bit(symbolic.c_str()));
			if (symbolic == current) {
				currentIdx = idx;
			}
			++idx;
		}
		if (currentIdx >= 0) {
			widget->setCurrentIndex(currentIdx);
		}
		configureEditorWidget(widget);
		QObject::connect(widget, &QComboBox::currentTextChanged, [feature](const QString& text) {
			const auto value = text.toStdString();
			feature->SetValue(value.c_str());
		});
		return widget;
	}
	case VmbFeatureDataBool: {
		bool state = false;
		feature->GetValue(state);
		auto* widget = new QCheckBox;
		widget->setChecked(state);
		widget->setEnabled(writable);
		configureEditorWidget(widget);
		QObject::connect(widget, &QCheckBox::toggled, [feature](bool checked) {
			feature->SetValue(checked);
		});
		return widget;
	}
	case VmbFeatureDataString: {
		std::string value;
		feature->GetValue(value);
		auto* widget = new QLineEdit(QString::fromStdString(value));
		widget->setEnabled(writable);
		configureEditorWidget(widget);
		QObject::connect(widget, &QLineEdit::editingFinished, [feature, widget]() {
			const auto value = widget->text().toStdString();
			feature->SetValue(value.c_str());
		});
		return widget;
	}
	case VmbFeatureDataCommand: {
		auto* widget = new QPushButton(tr("执行"));
		widget->setEnabled(writable);
		configureEditorWidget(widget);
		QObject::connect(widget, &QPushButton::clicked, [feature, this]() {
			VmbErrorType err = feature->RunCommand();
			if (err != VmbErrorSuccess) {
				Q_EMIT logMsg(tr("命令执行失败: 错误码 %1").arg(static_cast<int>(err)));
			}
		});
		return widget;
	}
	default:
		break;
	}

	return nullptr;
}

void FeaturePanel::configureEditorWidget(QWidget* widget) const {
	if (!widget) {
		return;
	}
	widget->setFocusPolicy(Qt::StrongFocus);
	widget->installEventFilter(new WheelBlocker(widget));
}
