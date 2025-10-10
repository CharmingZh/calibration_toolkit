#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "camera/CameraWindow.h"
#include "camera/FeaturePanel.h"
#include "camera/ImageView.h"
#include "camera/focus/FocusSummaryPanel.h"
#include "camera/StatusDashboard.h"
#include "camera/Utils.h"
#include "camera/VimbaController.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <cmath>
#include <QDate>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMap>
#include <QMessageBox>
#include <QMargins>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QShowEvent>
#include <QSplitter>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVariantMap>
#include <QVBoxLayout>
#include <QStringList>
#include <QWidget>
#include <QDateTime>

#include <algorithm>
#include <QtConcurrent/QtConcurrentRun>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

CameraWindow::CameraWindow(QWidget* parent)
	: QWidget(parent)
	, m_cam(new VimbaController(this))
{
	buildUi();
	resetFocusPanel();

	connect(m_cam, &VimbaController::frameReady, this, &CameraWindow::onFrame);
	connect(m_cam, &VimbaController::statsUpdated, this, &CameraWindow::onStats);
	connect(m_cam, &VimbaController::cameraOpened, this, [this](const QString& id, const QString& model) {
		resetFocusPanel();
		updateConnectionBanner(true, id, model);
		if (m_panel) {
			m_panel->setCamera(m_cam->camera());
		}
		m_streaming = false;
		Q_EMIT streamingStateChanged(false);
		updateActionStates();

		QStringList statusParts;
		const QString descriptor = model.isEmpty() ? id : model;
		if (!descriptor.isEmpty()) {
			statusParts << tr("已连接 %1").arg(descriptor);
		} else {
			statusParts << tr("相机已连接");
		}

		QString configMessage;
		const QString configDir = resolveConfigDirectory();
		if (!configDir.isEmpty() && m_cam->applyConfigurationProfile(configDir, id, &configMessage)) {
			if (!configMessage.isEmpty()) {
				statusParts << configMessage;
			} else {
				statusParts << tr("已加载默认配置");
			}
			if (m_panel) {
				m_panel->refresh();
			}
		}

		flashStatus(statusParts.join(QStringLiteral(" · ")), 3200);
	});
	connect(m_cam, &VimbaController::cameraClosed, this, [this]() {
		if (m_panel) {
			m_panel->setCamera(VmbCPP::CameraPtr());
		}
		m_streaming = false;
		m_latestFps = 0.0;
		m_latestBandwidth = 0.0;
		updateConnectionBanner(false);
		updateActionStates();
		Q_EMIT streamingStateChanged(false);
		if (m_status) {
			m_status->setText(tr("FPS: --  |  带宽: --"));
		}
		resetFocusPanel();
	});
	connect(m_cam, &VimbaController::errorOccured, this, [this](const QString& msg) {
		flashStatus(msg, 4000);
	});

	reloadCameraList();
	updateActionStates();
	applyEmbeddedMode();

	connect(&m_focusWatcher, &QFutureWatcher<FocusEvaluator::Metrics>::finished,
		this, &CameraWindow::handleFocusMetricsReady);
}

CameraWindow::~CameraWindow() {
	if (m_cam) {
		if (m_streaming) {
			m_cam->stop();
		}
		if (m_cam->camera()) {
			m_cam->close();
		}
	}
	resetFocusPanel();
	if (m_focusWatcher.isRunning()) {
		m_focusWatcher.cancel();
		m_focusWatcher.waitForFinished();
	}
}

void CameraWindow::setSnapshotDirectory(const QString& directory)
{
	m_snapshotDir = directory;
}

void CameraWindow::setSnapshotNamingPrefix(const QString& prefix)
{
	const QString trimmed = prefix.trimmed();
	m_snapshotPrefix = trimmed.isEmpty() ? QStringLiteral("snap") : trimmed;
}
void CameraWindow::setSnapshotPathProvider(const std::function<QString()>& provider)
{
	m_snapshotPathProvider = provider;
}

void CameraWindow::triggerSnapshot()
{
	onSnap();
}

void CameraWindow::connectSelectedCamera()
{
	onOpen();
}

void CameraWindow::disconnectCamera()
{
	onClose();
}

void CameraWindow::startStreaming()
{
	onStart();
}

void CameraWindow::stopStreaming()
{
	onStop();
}

void CameraWindow::refreshCameraList()
{
	reloadCameraList();
	updateActionStates();
}

void CameraWindow::setEmbeddedMode(bool embedded)
{
	if (m_embeddedMode == embedded) {
		return;
	}
	m_embeddedMode = embedded;
	applyEmbeddedMode();
}

void CameraWindow::applyEmbeddedMode()
{
	const bool embedded = m_embeddedMode;

	if (m_primaryToolbar) {
		m_primaryToolbar->setVisible(!embedded);
	}

	if (m_headerPanel) {
		m_headerPanel->setVisible(!embedded);
	}

	if (m_mainLayout) {
		if (embedded) {
			m_mainLayout->setContentsMargins(12, 12, 12, 8);
			m_mainLayout->setSpacing(12);
		} else {
			m_mainLayout->setContentsMargins(24, 24, 24, 12);
			m_mainLayout->setSpacing(20);
		}
	}

	if (m_inspectorPane) {
		if (auto *layout = qobject_cast<QVBoxLayout *>(m_inspectorPane->layout())) {
			if (embedded) {
				layout->setContentsMargins(12, 12, 12, 12);
				layout->setSpacing(12);
			} else {
				layout->setContentsMargins(18, 24, 18, 18);
				layout->setSpacing(18);
			}
		}
	}

	if (m_featureCard) {
		m_featureCard->setVisible(!embedded);
	}

	if (m_statusFrame) {
		m_statusFrame->setContentsMargins(embedded ? QMargins(12, 6, 12, 6)
												   : QMargins(24, 8, 24, 8));
	}

	if (embedded) {
		m_splitterPresetApplied = false;
		applySplitterPreset();
	}
}

void CameraWindow::applySplitterPreset()
{
	if (!m_splitter) {
		return;
	}

	const double ratio = m_embeddedMode ? 0.52 : 0.6;
	const int totalWidth = m_splitter->width() > 0 ? m_splitter->width() : width();
	const int fallback = totalWidth > 0 ? totalWidth : 1200;
	const int left = static_cast<int>(fallback * ratio);
	const int right = std::max(280, fallback - left);

	m_splitter->setSizes({left, right});
	m_splitterPresetApplied = true;
}

QString CameraWindow::resolveConfigDirectory() const
{
	QStringList candidates;
	const QString appDir = QCoreApplication::applicationDirPath();
	if (!appDir.isEmpty()) {
		QDir dir(appDir);
		candidates << dir.filePath(QStringLiteral("config"));
		candidates << dir.filePath(QStringLiteral("../config"));
		candidates << dir.filePath(QStringLiteral("../Resources/config"));
		candidates << dir.filePath(QStringLiteral("../../config"));
		candidates << dir.filePath(QStringLiteral("../../../config"));
	}

	QDir currentDir = QDir::current();
	candidates << currentDir.absoluteFilePath(QStringLiteral("config"));
	candidates << currentDir.absoluteFilePath(QStringLiteral("../config"));

	QString resolved;
	for (const QString& candidate : candidates) {
		QDir path(candidate);
		if (path.exists()) {
			resolved = path.absolutePath();
			break;
		}
	}
	return resolved;
}

void CameraWindow::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	if (!m_splitterPresetApplied) {
		QTimer::singleShot(0, this, [this]() { applySplitterPreset(); });
	}
}

void CameraWindow::buildUi() {
	setObjectName(QStringLiteral("CameraWindow"));
	setWindowTitle(tr("Alvium Vision Studio"));
	setMinimumSize(1100, 780);

	m_rootLayout = new QVBoxLayout(this);
	m_rootLayout->setContentsMargins(0, 0, 0, 0);
	m_rootLayout->setSpacing(0);

	m_primaryToolbar = new QToolBar(tr("主控"), this);
	m_primaryToolbar->setObjectName(QStringLiteral("PrimaryToolbar"));
	m_primaryToolbar->setMovable(false);
	m_primaryToolbar->setFloatable(false);
	m_primaryToolbar->setAllowedAreas(Qt::TopToolBarArea);
	m_primaryToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	m_primaryToolbar->setIconSize(QSize(22, 22));
	m_primaryToolbar->setContextMenuPolicy(Qt::PreventContextMenu);
	m_rootLayout->addWidget(m_primaryToolbar, 0);

	m_actOpen = new QAction(QIcon(QStringLiteral(":/icons/camera_open.svg")), tr("连接"), this);
	connect(m_actOpen, &QAction::triggered, this, &CameraWindow::onOpen);
	m_primaryToolbar->addAction(m_actOpen);

	m_actClose = new QAction(QIcon(QStringLiteral(":/icons/camera_close.svg")), tr("断开"), this);
	connect(m_actClose, &QAction::triggered, this, &CameraWindow::onClose);
	m_primaryToolbar->addAction(m_actClose);

	m_primaryToolbar->addSeparator();

	m_actStart = new QAction(QIcon(QStringLiteral(":/icons/stream_start.svg")), tr("开始取流"), this);
	m_actStart->setCheckable(true);
	connect(m_actStart, &QAction::triggered, this, &CameraWindow::onStart);
	m_primaryToolbar->addAction(m_actStart);

	m_actStop = new QAction(QIcon(QStringLiteral(":/icons/stream_stop.svg")), tr("停止取流"), this);
	m_actStop->setCheckable(true);
	connect(m_actStop, &QAction::triggered, this, &CameraWindow::onStop);
	m_primaryToolbar->addAction(m_actStop);

	m_actSnap = new QAction(QIcon(QStringLiteral(":/icons/snapshot.svg")), tr("快照"), this);
	connect(m_actSnap, &QAction::triggered, this, &CameraWindow::onSnap);
	m_primaryToolbar->addAction(m_actSnap);

	QAction* actRefresh = new QAction(QIcon(QStringLiteral(":/icons/refresh.svg")), tr("刷新列表"), this);
	connect(actRefresh, &QAction::triggered, this, &CameraWindow::onRefreshCameras);
	m_primaryToolbar->addAction(actRefresh);

	m_actTuningTimeline = new QAction(QIcon(QStringLiteral(":/icons/evaluate.svg")), tr("调参时间线"), this);
	connect(m_actTuningTimeline, &QAction::triggered, this, [this]() {
		Q_EMIT tuningTimelineRequested();
	});
	m_primaryToolbar->addAction(m_actTuningTimeline);

	m_primaryToolbar->addSeparator();

	m_actFrameAssist = new QAction(QIcon(QStringLiteral(":/icons/frame_rate.svg")), tr("帧率助手"), this);
	connect(m_actFrameAssist, &QAction::triggered, this, &CameraWindow::showFrameRateAssistant);
	m_primaryToolbar->addAction(m_actFrameAssist);

	m_actAbout = new QAction(QIcon(QStringLiteral(":/icons/info.svg")), tr("关于"), this);
	connect(m_actAbout, &QAction::triggered, this, &CameraWindow::showAboutDialog);
	m_primaryToolbar->addAction(m_actAbout);

	m_primaryToolbar->addSeparator();

	m_cameraCombo = new QComboBox(m_primaryToolbar);
	m_cameraCombo->setObjectName(QStringLiteral("CameraSelector"));
	m_cameraCombo->setEnabled(false);
	m_cameraCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	m_cameraCombo->setInsertPolicy(QComboBox::NoInsert);
	m_cameraCombo->addItem(tr("未检测到相机"));
	m_primaryToolbar->addWidget(m_cameraCombo);

	auto* spacer = new QWidget(m_primaryToolbar);
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	m_primaryToolbar->addWidget(spacer);

	m_cameraBadge = new QLabel(tr("离线"), m_primaryToolbar);
	m_cameraBadge->setObjectName(QStringLiteral("CameraBadge"));
	m_cameraBadge->setProperty("connected", false);
	m_cameraBadge->setProperty("streaming", false);
	m_cameraBadge->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
	m_cameraBadge->setMinimumWidth(170);
	m_primaryToolbar->addWidget(m_cameraBadge);

	m_splitter = new QSplitter(Qt::Horizontal, this);
	m_splitter->setObjectName(QStringLiteral("CameraSplitter"));
	m_splitter->setChildrenCollapsible(false);
	m_rootLayout->addWidget(m_splitter, 1);

	auto* mainPane = new QWidget(m_splitter);
	m_mainLayout = new QVBoxLayout(mainPane);
	m_mainLayout->setContentsMargins(24, 24, 24, 12);
	m_mainLayout->setSpacing(20);

	auto* topRow = new QHBoxLayout();
	topRow->setContentsMargins(0, 0, 0, 0);
	topRow->setSpacing(18);

	m_headerPanel = new QWidget(mainPane);
	m_headerPanel->setObjectName(QStringLiteral("HeaderPanel"));
	auto* headerLayout = new QVBoxLayout(m_headerPanel);
	headerLayout->setContentsMargins(0, 0, 0, 0);
	headerLayout->setSpacing(12);

	auto* titleLabel = new QLabel(tr("Alvium Vision Studio"), m_headerPanel);
	titleLabel->setObjectName(QStringLiteral("HeroTitle"));
	QFont titleFont = titleLabel->font();
	titleFont.setPointSizeF(titleFont.pointSizeF() + 6.0);
	titleFont.setBold(true);
	titleLabel->setFont(titleFont);
	headerLayout->addWidget(titleLabel);

	m_metaLabel = new QLabel(tr("实时监控 · 分级调参 · 深度诊断"), m_headerPanel);
	m_metaLabel->setObjectName(QStringLiteral("HeroMeta"));
	m_metaLabel->setWordWrap(true);
	headerLayout->addWidget(m_metaLabel);

	m_hintLabel = new QLabel(tr("通过顶部控制连接相机，右侧仪表盘将即时反馈关键指标。"), m_headerPanel);
	m_hintLabel->setObjectName(QStringLiteral("HeroHint"));
	m_hintLabel->setWordWrap(true);
	headerLayout->addWidget(m_hintLabel);

	m_checklistLabel = new QLabel(m_headerPanel);
	m_checklistLabel->setObjectName(QStringLiteral("HeroChecklist"));
	m_checklistLabel->setTextFormat(Qt::RichText);
	m_checklistLabel->setWordWrap(true);
	m_checklistLabel->setStyleSheet(QStringLiteral("color: rgba(210, 223, 245, 0.74); font-size: 11px; line-height: 1.5;"));
	m_checklistLabel->setText(tr("<ol style='margin:0;padding-left:18px;'>"
	                     "<li>调整曝光与增益，保持灰度不过曝并关注实时直方图。</li>"
	                     "<li>通过“帧率助手”确认链路带宽，将目标 FPS 与采集帧率对齐。</li>"
	                     "<li>参数稳定后点击“快照”，系统会记录调参影像与当前指标。</li>"
	                     "</ol>"));
	headerLayout->addWidget(m_checklistLabel);
	headerLayout->addStretch(1);

	m_dashboard = new StatusDashboard(mainPane);
	topRow->addWidget(m_headerPanel, 3);
	topRow->addWidget(m_dashboard, 4);
	topRow->setStretch(0, 3);
	topRow->setStretch(1, 4);
	m_mainLayout->addLayout(topRow);

	auto* viewerCard = new QFrame(mainPane);
	viewerCard->setObjectName(QStringLiteral("ViewerCard"));
	viewerCard->setMinimumHeight(540);
	auto* viewerLayout = new QGridLayout(viewerCard);
	viewerLayout->setContentsMargins(24, 24, 24, 24);
	viewerLayout->setSpacing(0);

	m_view = new ImageView(viewerCard);
	m_view->setObjectName(QStringLiteral("LiveView"));
	m_view->setMinimumHeight(500);
	viewerLayout->addWidget(m_view, 0, 0);
	connect(m_view, &ImageView::roiChanged, this, &CameraWindow::onRoiChanged);

	m_statsBadge = new QLabel(tr("等待帧…"), viewerCard);
	m_statsBadge->setObjectName(QStringLiteral("StatsBadge"));
	m_statsBadge->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	viewerLayout->addWidget(m_statsBadge, 0, 0, Qt::AlignTop | Qt::AlignLeft);

	auto* viewerShadow = new QGraphicsDropShadowEffect(viewerCard);
	viewerShadow->setBlurRadius(40);
	viewerShadow->setOffset(0, 20);
	viewerShadow->setColor(QColor(12, 20, 34, 180));
	viewerCard->setGraphicsEffect(viewerShadow);

	m_mainLayout->addWidget(viewerCard, 1);
	m_mainLayout->setStretch(0, 0);
	m_mainLayout->setStretch(1, 1);
	m_splitter->addWidget(mainPane);

	m_inspectorPane = new QWidget(m_splitter);
	m_inspectorPane->setObjectName(QStringLiteral("InspectorPane"));
	auto* inspectorLayout = new QVBoxLayout(m_inspectorPane);
	inspectorLayout->setContentsMargins(18, 24, 18, 18);
	inspectorLayout->setSpacing(18);

	m_focusPanel = new FocusSummaryPanel(m_inspectorPane);
	m_focusPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	inspectorLayout->addWidget(m_focusPanel, 0);

	auto* featureCard = new QFrame(m_inspectorPane);
	featureCard->setObjectName(QStringLiteral("FeatureCard"));
	m_featureCard = featureCard;
	auto* featureLayout = new QVBoxLayout(featureCard);
	featureLayout->setContentsMargins(0, 0, 0, 0);
	featureLayout->setSpacing(0);

	m_panel = new FeaturePanel(VmbCPP::CameraPtr(), featureCard);
	m_panel->setObjectName(QStringLiteral("FeaturePanel"));
	featureLayout->addWidget(m_panel);

	inspectorLayout->addWidget(featureCard, 1);
	m_splitter->addWidget(m_inspectorPane);
	m_splitter->setStretchFactor(0, 1);
	m_splitter->setStretchFactor(1, 1);
	m_splitter->setSizes({600, 600});

	connect(m_panel, &FeaturePanel::logMsg, this, [this](const QString& msg) {
		flashStatus(msg, 4000);
	});

	m_statusFrame = new QFrame(this);
	m_statusFrame->setObjectName(QStringLiteral("StatusFrame"));
	auto* statusLayout = new QHBoxLayout(m_statusFrame);
	statusLayout->setContentsMargins(24, 8, 24, 8);
	statusLayout->setSpacing(12);

	m_status = new QLabel(tr("FPS: --  |  带宽: --"), m_statusFrame);
	m_status->setObjectName(QStringLiteral("StatusLabel"));
	statusLayout->addWidget(m_status, 0, Qt::AlignLeft);

	statusLayout->addStretch(1);

	m_statusFlash = new QLabel(m_statusFrame);
	m_statusFlash->setObjectName(QStringLiteral("StatusFlash"));
	m_statusFlash->setVisible(false);
	statusLayout->addWidget(m_statusFlash, 0, Qt::AlignRight);

	m_rootLayout->addWidget(m_statusFrame, 0);

	setStyleSheet(QStringLiteral(R"(
QToolBar#PrimaryToolbar {
	background: rgba(9, 14, 21, 0.95);
	border: none;
	padding: 10px 24px;
	spacing: 12px;
}
QToolBar#PrimaryToolbar QToolButton {
	color: #dee6f0;
	font-size: 12px;
	font-weight: 600;
	padding: 8px 14px;
	border-radius: 12px;
	transition: background 120ms ease;
}
QToolBar#PrimaryToolbar QToolButton:hover {
	background: rgba(80, 143, 255, 0.26);
}
QToolBar#PrimaryToolbar QToolButton:pressed {
	background: rgba(80, 143, 255, 0.4);
}
QToolBar#PrimaryToolbar QToolButton:disabled {
	color: rgba(180, 192, 210, 0.45);
	background: transparent;
}
QToolBar#PrimaryToolbar QToolButton[active="true"] {
	background: rgba(80, 143, 255, 0.58);
	color: #f3f8ff;
	border: 1px solid rgba(120, 196, 255, 0.7);
}
QToolBar#PrimaryToolbar QLabel#CameraBadge {
	padding: 8px 20px;
	font-size: 13px;
	font-weight: 700;
	border-radius: 999px;
	border: 1px solid rgba(120, 196, 255, 0.45);
	min-width: 190px;
	color: #0e233e;
}
QToolBar#PrimaryToolbar QLabel#CameraBadge[connected="false"] {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 rgba(255, 136, 150, 0.32), stop:1 rgba(255, 78, 127, 0.28));
	color: #fff3f6;
	border-color: rgba(255, 164, 188, 0.55);
}
QToolBar#PrimaryToolbar QLabel#CameraBadge[connected="true"][streaming="false"] {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 rgba(96, 168, 255, 0.32), stop:1 rgba(150, 208, 255, 0.28));
	color: #0c233e;
	border-color: rgba(104, 186, 255, 0.6);
}
QToolBar#PrimaryToolbar QLabel#CameraBadge[streaming="true"] {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 rgba(64, 238, 210, 0.34), stop:1 rgba(92, 255, 192, 0.28));
	color: #03281d;
	border-color: rgba(94, 240, 198, 0.6);
}
#CameraSelector {
	font-size: 12px;
	color: #d8e2f1;
	padding: 6px 10px;
	min-height: 32px;
	border-radius: 10px;
	background: rgba(24, 32, 44, 0.9);
	border: 1px solid rgba(88, 172, 255, 0.35);
}
#CameraSelector:disabled {
	color: rgba(180, 192, 210, 0.6);
	border-color: rgba(128, 138, 155, 0.3);
}
#HeaderPanel {
	background: transparent;
}
#HeroTitle {
	color: #f3f7ff;
	letter-spacing: 0.8px;
	font-size: 24px;
	font-weight: 800;
}
#HeroMeta {
	color: rgba(218, 230, 255, 0.92);
	font-size: 15px;
	font-weight: 600;
}
#HeroHint {
	color: rgba(210, 223, 245, 0.82);
	font-size: 13px;
	line-height: 1.65;
}
#HeroChecklist {
	color: rgba(198, 212, 248, 0.86);
	font-size: 12.6px;
	line-height: 1.7;
}
#ViewerCard {
	background: #0d1117;
	border-radius: 22px;
}
#StatsBadge {
	background: rgba(24, 36, 58, 0.86);
	color: #a6c7ff;
	border-radius: 14px;
	padding: 10px 18px;
	font-weight: 700;
	border: 1px solid rgba(120, 170, 255, 0.45);
	letter-spacing: 0.4px;
}
#StatsBadge[streaming="true"] {
	background: rgba(22, 64, 52, 0.88);
	color: #7effd0;
	border-color: rgba(88, 220, 182, 0.55);
}
QWidget#InspectorPane {
	background: rgba(10, 14, 21, 0.96);
}
QFrame#FeatureCard {
	background: rgba(16, 22, 34, 0.95);
	border-radius: 18px;
	border: 1px solid rgba(72, 132, 255, 0.18);
	padding: 12px;
}
QWidget#FeaturePanel {
	background: transparent;
	color: #d5e0ff;
}
QScrollArea {
	border: none;
}
FeaturePanel QLabel {
	color: #d4deee;
	font-weight: 500;
}
FeaturePanel QFrame#FeatureSection {
	background: rgba(16, 24, 36, 0.86);
	border-radius: 14px;
	border: 1px solid rgba(80, 143, 255, 0.18);
	padding: 14px 18px 18px;
}
FeaturePanel QLabel#FeatureSectionHeader {
	color: #8ec5ff;
	font-size: 13px;
	font-weight: 700;
	letter-spacing: 0.35px;
	margin-bottom: 10px;
}
FeaturePanel QLineEdit,
FeaturePanel QDoubleSpinBox,
FeaturePanel QSpinBox,
FeaturePanel QComboBox,
FeaturePanel QCheckBox,
FeaturePanel QPushButton {
	background: rgba(17, 24, 34, 0.86);
	border: 1px solid rgba(88, 172, 255, 0.2);
	border-radius: 10px;
	color: #e5ecf8;
	padding: 6px 8px;
}
FeaturePanel QCheckBox::indicator {
	width: 16px;
	height: 16px;
}
FeaturePanel QPushButton {
	padding: 8px 14px;
	font-weight: 600;
	color: #7bd8ff;
}
FeaturePanel QPushButton:hover {
	background: rgba(80, 143, 255, 0.24);
}
QFrame#StatusFrame {
	background: rgba(10, 14, 21, 0.94);
	border-top: 1px solid rgba(72, 132, 255, 0.18);
}
QLabel#StatusLabel {
	color: #8ea2c0;
	font-size: 11px;
	font-weight: 500;
}
QLabel#StatusFlash {
	color: #b8ccff;
	font-size: 11px;
}
)"));

	m_statusTimer = new QTimer(this);
	m_statusTimer->setInterval(1500);
	connect(m_statusTimer, &QTimer::timeout, this, &CameraWindow::pollCameraStatus);
	m_statusTimer->start();

	updateConnectionBanner(false);
}

void CameraWindow::reloadCameraList() {
	if (!m_cameraCombo) {
		return;
	}

	const QString previous = m_cameraCombo->currentText();
	const QStringList cams = m_cam ? m_cam->listCameras() : QStringList{};

	QSignalBlocker blocker(m_cameraCombo);
	m_cameraCombo->clear();

	if (cams.isEmpty()) {
		m_cameraCombo->addItem(tr("未检测到相机"));
		m_cameraCombo->setEnabled(false);
	} else {
		m_cameraCombo->addItems(cams);
		m_cameraCombo->setEnabled(true);
		int idx = -1;
		if (!previous.isEmpty()) {
			idx = m_cameraCombo->findText(previous, Qt::MatchFixedString);
		}
		if (idx < 0 && !m_lastCameraId.isEmpty()) {
			idx = m_cameraCombo->findText(m_lastCameraId, Qt::MatchFixedString);
		}
		if (idx >= 0) {
			m_cameraCombo->setCurrentIndex(idx);
		}
	}
}

void CameraWindow::updateConnectionBanner(bool connected, const QString& id, const QString& model) {
	if (!m_cameraBadge) {
		return;
	}

	const QString newId = connected ? id : QString();
	const QString newModel = connected ? model : QString();
	const bool stateChanged = (m_connected != connected)
		|| (connected && (newId != m_lastCameraId || newModel != m_lastCameraModel));

	if (connected) {
		m_lastCameraId = newId;
		m_lastCameraModel = newModel;
	} else {
		m_lastCameraId.clear();
		m_lastCameraModel.clear();
	}

	QString descriptor;
	if (connected) {
		if (!model.isEmpty() && !id.isEmpty()) {
			descriptor = QStringLiteral("%1  ·  %2").arg(model, id);
		} else {
			descriptor = model.isEmpty() ? id : model;
		}
		m_cameraBadge->setText(tr("在线 · %1").arg(descriptor));
		if (m_cameraCombo && !id.isEmpty()) {
			const int idx = m_cameraCombo->findText(id, Qt::MatchFixedString);
			if (idx >= 0) {
				m_cameraCombo->setCurrentIndex(idx);
			}
		}
	} else {
		m_cameraBadge->setText(tr("离线"));
	}

	m_cameraBadge->setProperty("connected", connected);
	if (!connected) {
		m_cameraBadge->setProperty("streaming", false);
	}
	if (auto* s = m_cameraBadge->style()) {
		s->unpolish(m_cameraBadge);
		s->polish(m_cameraBadge);
	}

	pollCameraStatus();

	if (stateChanged) {
		Q_EMIT connectionStateChanged(connected, connected ? m_lastCameraId : QString(), connected ? m_lastCameraModel : QString());
	}

	m_connected = connected;
}

void CameraWindow::updateActionStates() {
	const bool hasCamera = m_cam && m_cam->camera();
	if (m_actOpen) {
		m_actOpen->setEnabled(!m_streaming);
	}
	if (m_actClose) {
		m_actClose->setEnabled(hasCamera && !m_streaming);
	}
	if (m_actStart) {
		m_actStart->setEnabled(hasCamera && !m_streaming);
		m_actStart->setChecked(m_streaming);
	}
	if (m_actStop) {
		m_actStop->setEnabled(hasCamera && m_streaming);
		m_actStop->setChecked(hasCamera && m_streaming);
	}
	if (m_actSnap) {
		m_actSnap->setEnabled(hasCamera);
	}
	if (m_actFrameAssist) {
		m_actFrameAssist->setEnabled(hasCamera);
	}

	if (auto* toolbar = findChild<QToolBar*>("PrimaryToolbar")) {
		auto applyActive = [toolbar](QAction* action, bool active) {
			if (!action) {
				return;
			}
			if (QWidget* button = toolbar->widgetForAction(action)) {
				button->setProperty("active", active);
				if (auto* toolButton = qobject_cast<QToolButton*>(button)) {
					toolButton->setChecked(active);
				}
				if (auto* style = button->style()) {
					style->unpolish(button);
					style->polish(button);
				}
			}
		};
		applyActive(m_actStart, m_streaming);
		applyActive(m_actStop, hasCamera && m_streaming);
	}

	if (m_statsBadge) {
		m_statsBadge->setProperty("streaming", m_streaming);
		if (auto* s = m_statsBadge->style()) {
			s->unpolish(m_statsBadge);
			s->polish(m_statsBadge);
		}
	}

	if (m_cameraBadge) {
		m_cameraBadge->setProperty("streaming", hasCamera && m_streaming);
		if (auto* s = m_cameraBadge->style()) {
			s->unpolish(m_cameraBadge);
			s->polish(m_cameraBadge);
		}
	}
}

void CameraWindow::flashStatus(const QString& message, int timeoutMs) {
	if (!m_statusFlash) {
		return;
	}
	m_statusFlash->setText(message);
	m_statusFlash->setVisible(true);
	if (!m_flashTimer) {
		m_flashTimer = new QTimer(this);
		m_flashTimer->setSingleShot(true);
		connect(m_flashTimer, &QTimer::timeout, this, [this]() {
			if (m_statusFlash) {
				m_statusFlash->clear();
				m_statusFlash->setVisible(false);
			}
		});
	}
	m_flashTimer->start(std::max(0, timeoutMs));
}

void CameraWindow::refreshDashboard() {
	if (!m_dashboard) {
		return;
	}

	QMap<QString, QString> metrics;
	if (m_cachedMetrics.isEmpty()) {
		metrics.insert(QStringLiteral("connection"), tr("离线"));
		metrics.insert(QStringLiteral("camera"), QStringLiteral("--"));
		metrics.insert(QStringLiteral("frameRate"), QStringLiteral("--"));
		metrics.insert(QStringLiteral("bandwidth"), QStringLiteral("--"));
		metrics.insert(QStringLiteral("acqFrameRate"), QStringLiteral("--"));
		metrics.insert(QStringLiteral("acqFrameRateEnable"), QStringLiteral("--"));
		metrics.insert(QStringLiteral("exposure"), QStringLiteral("--"));
		metrics.insert(QStringLiteral("resolution"), QStringLiteral("--"));
		metrics.insert(QStringLiteral("pixelFormat"), QStringLiteral("--"));
		metrics.insert(QStringLiteral("stream"), QStringLiteral("--"));
	} else {
		for (auto it = m_cachedMetrics.cbegin(); it != m_cachedMetrics.cend(); ++it) {
			metrics.insert(it.key(), it.value().toString());
		}
	}

	m_dashboard->setMetrics(metrics);
}

void CameraWindow::pollCameraStatus() {
	QVariantMap newMetrics;
	const bool connected = m_cam && m_cam->camera();
	newMetrics.insert(QStringLiteral("connection"), connected ? tr("在线") : tr("离线"));

	QString cameraInfo;
	if (connected) {
		QString model = m_lastCameraModel;
		QString id = m_lastCameraId;
		if ((model.isEmpty() || id.isEmpty()) && m_cam) {
			if (auto cam = m_cam->camera()) {
				std::string rawModel;
				std::string rawId;
				cam->GetModel(rawModel);
				cam->GetID(rawId);
				if (model.isEmpty() && !rawModel.empty()) {
					model = QString::fromStdString(rawModel);
				}
				if (id.isEmpty() && !rawId.empty()) {
					id = QString::fromStdString(rawId);
				}
			}
		}
		if (!model.isEmpty() && !id.isEmpty()) {
			cameraInfo = QStringLiteral("%1  ·  %2").arg(model, id);
		} else if (!model.isEmpty()) {
			cameraInfo = model;
		} else if (!id.isEmpty()) {
			cameraInfo = id;
		}
	}
	newMetrics.insert(QStringLiteral("camera"), cameraInfo.isEmpty() ? QStringLiteral("--") : cameraInfo);

	if (connected && m_latestFps > 0.0) {
		newMetrics.insert(QStringLiteral("frameRate"), tr("%1 FPS").arg(QString::number(m_latestFps, 'f', 1)));
	} else {
		newMetrics.insert(QStringLiteral("frameRate"), QStringLiteral("--"));
	}

	if (connected && m_latestBandwidth > 0.0) {
		newMetrics.insert(QStringLiteral("bandwidth"), Utils::bytesHumanReadable(m_latestBandwidth));
	} else {
		newMetrics.insert(QStringLiteral("bandwidth"), QStringLiteral("--"));
	}

	if (connected) {
		const QString acqRate = readFeatureValue("AcquisitionFrameRate");
		if (!acqRate.isEmpty()) {
			newMetrics.insert(QStringLiteral("acqFrameRate"), tr("%1 FPS").arg(acqRate));
		} else {
			newMetrics.insert(QStringLiteral("acqFrameRate"), QStringLiteral("--"));
		}

		const QString acqEnable = readBoolDisplay("AcquisitionFrameRateEnable", tr("已启用"), tr("由相机自动控制"));
		newMetrics.insert(QStringLiteral("acqFrameRateEnable"), acqEnable.isEmpty() ? QStringLiteral("--") : acqEnable);

		QString exposure = readFeatureValue("ExposureTime");
		if (!exposure.isEmpty()) {
			bool ok = false;
			double micros = exposure.toDouble(&ok);
			if (ok) {
				double ms = micros / 1000.0;
				exposure = (ms >= 1.0)
					? tr("%1 ms").arg(QString::number(ms, 'f', ms < 10.0 ? 2 : 1))
					: tr("%1 μs").arg(QString::number(micros, 'f', 0));
			}
		}
		newMetrics.insert(QStringLiteral("exposure"), exposure.isEmpty() ? QStringLiteral("--") : exposure);

		const QString width = readFeatureValue("Width");
		const QString height = readFeatureValue("Height");
		QString resolution;
		if (!width.isEmpty() && !height.isEmpty()) {
			resolution = QStringLiteral("%1 × %2").arg(width, height);
		}
		newMetrics.insert(QStringLiteral("resolution"), resolution.isEmpty() ? QStringLiteral("--") : resolution);

		QString pixel = readEnumDisplay("PixelFormat");
		newMetrics.insert(QStringLiteral("pixelFormat"), pixel.isEmpty() ? QStringLiteral("--") : pixel);

		QString streamBw;
		QString streamValue = readFeatureValue("StreamBytesPerSecond");
		if (!streamValue.isEmpty()) {
			bool ok = false;
			double numeric = streamValue.toDouble(&ok);
			if (ok) {
				streamBw = Utils::bytesHumanReadable(numeric);
			}
		}
		if (streamBw.isEmpty()) {
			streamValue = readFeatureValue("DeviceStreamChannelPacketSize");
			if (!streamValue.isEmpty()) {
				streamBw = tr("包长 %1 B").arg(streamValue);
			}
		}
		newMetrics.insert(QStringLiteral("stream"), streamBw.isEmpty() ? QStringLiteral("--") : streamBw);
	} else {
		newMetrics.insert(QStringLiteral("acqFrameRate"), QStringLiteral("--"));
		newMetrics.insert(QStringLiteral("acqFrameRateEnable"), QStringLiteral("--"));
		newMetrics.insert(QStringLiteral("exposure"), QStringLiteral("--"));
		newMetrics.insert(QStringLiteral("resolution"), QStringLiteral("--"));
		newMetrics.insert(QStringLiteral("pixelFormat"), QStringLiteral("--"));
		newMetrics.insert(QStringLiteral("stream"), QStringLiteral("--"));
	}

	const bool changed = (newMetrics != m_cachedMetrics);
	if (changed || m_cachedMetrics.isEmpty()) {
		m_cachedMetrics = newMetrics;
		refreshDashboard();
	}
}

QVariantMap CameraWindow::currentSnapshotMetrics() const {
	QVariantMap metrics = m_cachedMetrics;
	metrics.insert(QStringLiteral("timestampUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	if (m_latestFps > 0.0) {
		metrics.insert(QStringLiteral("frameRateNumeric"), m_latestFps);
		const QString fpsText = tr("%1 FPS").arg(QString::number(m_latestFps, 'f', 2));
		metrics.insert(QStringLiteral("frameRate"), metrics.value(QStringLiteral("frameRate"), fpsText));
	}
	if (m_latestBandwidth > 0.0) {
		metrics.insert(QStringLiteral("bandwidthNumeric"), m_latestBandwidth);
		if (!metrics.contains(QStringLiteral("bandwidth"))) {
			metrics.insert(QStringLiteral("bandwidth"), Utils::bytesHumanReadable(m_latestBandwidth));
		}
	}

	auto captureNumericFeature = [this, &metrics](const char* featureName,
	                                             const QString& key,
	                                             const QString& unit,
	                                             int precision,
	                                             double scale) {
		const QString raw = readFeatureValue(featureName);
		if (raw.isEmpty()) {
			return;
		}
		bool ok = false;
		double numeric = raw.toDouble(&ok);
		if (!ok) {
			metrics.insert(key, raw);
			return;
		}
		double scaled = numeric * scale;
		QString formatted = unit.isEmpty()
			? QString::number(scaled, 'f', precision)
			: tr("%1 %2").arg(QString::number(scaled, 'f', precision), unit);
		metrics.insert(key, formatted);
		metrics.insert(key + QStringLiteral("Numeric"), scaled);
	};

	// Exposure time is reported in microseconds; convert to milliseconds when appropriate.
	captureNumericFeature("ExposureTime", QStringLiteral("exposure"), QStringLiteral("ms"), 2, 0.001);
	captureNumericFeature("Gain", QStringLiteral("gain"), QStringLiteral("dB"), 1, 1.0);
	captureNumericFeature("Gamma", QStringLiteral("gamma"), QString(), 2, 1.0);
	captureNumericFeature("BlackLevel", QStringLiteral("blackLevel"), QStringLiteral("DN"), 0, 1.0);

	if (m_focusPanel) {
		const QVariantMap focusMap = m_focusPanel->exportMetrics();
		for (auto it = focusMap.cbegin(); it != focusMap.cend(); ++it) {
			metrics.insert(it.key(), it.value());
		}
	}

	return metrics;
}

QString CameraWindow::readFeatureValue(const char* name) const {
	if (!m_cam || !name) {
		return {};
	}

	VmbCPP::FeaturePtr feature = m_cam->feature(name);
	if (!feature) {
		return {};
	}

	bool readable = false;
	if (feature->IsReadable(readable) != VmbErrorSuccess || !readable) {
		return {};
	}

	VmbFeatureDataType type = VmbFeatureDataUnknown;
	if (feature->GetDataType(type) != VmbErrorSuccess) {
		return {};
	}

	switch (type) {
	case VmbFeatureDataInt: {
		VmbInt64_t value = 0;
		if (feature->GetValue(value) == VmbErrorSuccess) {
			return QString::number(static_cast<qlonglong>(value));
		}
		break;
	}
	case VmbFeatureDataFloat: {
		double value = 0.0;
		if (feature->GetValue(value) == VmbErrorSuccess && std::isfinite(value)) {
			int precision = std::abs(value) < 1.0 ? 3 : 2;
			return QLocale::c().toString(value, 'f', precision);
		}
		break;
	}
	case VmbFeatureDataBool: {
		bool value = false;
		if (feature->GetValue(value) == VmbErrorSuccess) {
			return value ? QStringLiteral("true") : QStringLiteral("false");
		}
		break;
	}
	case VmbFeatureDataString: {
		std::string value;
		if (feature->GetValue(value) == VmbErrorSuccess) {
			return QString::fromStdString(value);
		}
		break;
	}
	case VmbFeatureDataEnum: {
		std::string value;
		if (feature->GetValue(value) == VmbErrorSuccess) {
			return QString::fromStdString(value);
		}
		break;
	}
	default:
		break;
	}

	return {};
}

QString CameraWindow::readEnumDisplay(const char* name) const {
	if (!m_cam || !name) {
		return {};
	}
	VmbCPP::FeaturePtr feature = m_cam->feature(name);
	if (!feature) {
		return {};
	}
	bool readable = false;
	if (feature->IsReadable(readable) != VmbErrorSuccess || !readable) {
		return {};
	}
	std::string current;
	if (feature->GetValue(current) != VmbErrorSuccess) {
		return {};
	}
	VmbCPP::EnumEntryVector entries;
	if (feature->GetEntries(entries) == VmbErrorSuccess) {
		for (const auto& entry : entries) {
			std::string symbol;
			entry.GetName(symbol);
			if (symbol == current) {
				std::string description;
				if (entry.GetDescription(description) == VmbErrorSuccess && !description.empty()) {
					return QString::fromStdString(description);
				}
				return QString::fromStdString(symbol);
			}
		}
	}
	return QString::fromStdString(current);
}

QString CameraWindow::readBoolDisplay(const char* name, const QString& trueText, const QString& falseText) const {
	if (!m_cam || !name) {
		return {};
	}
	VmbCPP::FeaturePtr feature = m_cam->feature(name);
	if (!feature) {
		return {};
	}
	bool readable = false;
	if (feature->IsReadable(readable) != VmbErrorSuccess || !readable) {
		return {};
	}
	bool value = false;
	if (feature->GetValue(value) == VmbErrorSuccess) {
		return value ? trueText : falseText;
	}
	return {};
}

void CameraWindow::showFrameRateAssistant() {
	if (!(m_cam && m_cam->camera())) {
		QMessageBox::information(this, tr("提示"), tr("请先连接相机以调整帧率"));
		return;
	}

	const char* frameRateCandidates[] = {"AcquisitionFrameRate", "AcquisitionFrameRateAbs"};
	VmbCPP::FeaturePtr frameRateFeature;
	for (const char* candidate : frameRateCandidates) {
		frameRateFeature = m_cam->feature(candidate);
		if (frameRateFeature) {
			break;
		}
	}

	VmbCPP::FeaturePtr enableFeature = m_cam->feature("AcquisitionFrameRateEnable");

	QDialog dialog(this);
	dialog.setWindowTitle(tr("帧率调节助手"));
	dialog.setModal(true);
	dialog.setMinimumWidth(420);

	auto* layout = new QVBoxLayout(&dialog);
	layout->setContentsMargins(20, 20, 20, 20);
	layout->setSpacing(16);

	auto* intro = new QLabel(tr("结合实时数据调整采集帧率，快速排查带宽瓶颈。"), &dialog);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	auto* form = new QFormLayout();
	form->setLabelAlignment(Qt::AlignRight);
	form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
	form->setHorizontalSpacing(18);
	form->setVerticalSpacing(12);
	layout->addLayout(form);

	auto* currentLabel = new QLabel(m_latestFps > 0.0
		? tr("%1 FPS (实时)").arg(QString::number(m_latestFps, 'f', 1))
		: tr("等待帧"), &dialog);
	form->addRow(tr("实时帧率"), currentLabel);

	auto* enableCheck = new QCheckBox(tr("启用采集帧率限制"), &dialog);
	form->addRow(QString(), enableCheck);

	auto* fpsSpin = new QDoubleSpinBox(&dialog);
	fpsSpin->setDecimals(2);
	fpsSpin->setRange(0.10, 1000.0);
	fpsSpin->setSingleStep(1.0);
	form->addRow(tr("目标帧率"), fpsSpin);

	bool frameRateWritable = false;
	double minRate = 0.0;
	double maxRate = 0.0;
	double currentRate = 0.0;
	if (frameRateFeature) {
		frameRateFeature->IsWritable(frameRateWritable);
		frameRateFeature->GetRange(minRate, maxRate);
		frameRateFeature->GetValue(currentRate);
	}

	if (frameRateWritable && maxRate > minRate) {
		fpsSpin->setRange(std::max(0.01, minRate), maxRate);
		fpsSpin->setSingleStep(std::max(0.01, (maxRate - minRate) / 100.0));
	}
	if (currentRate > 0.0) {
		fpsSpin->setValue(currentRate);
	}
	fpsSpin->setEnabled(frameRateFeature && frameRateWritable);

	bool enableWritable = false;
	bool enableDefault = true;
	if (enableFeature) {
		enableFeature->IsWritable(enableWritable);
		enableFeature->GetValue(enableDefault);
	}
	enableCheck->setChecked(enableDefault);
	enableCheck->setEnabled(enableFeature && enableWritable);

	auto* recommendation = new QLabel(&dialog);
	recommendation->setWordWrap(true);
	recommendation->setStyleSheet(QStringLiteral("color: #8eaed8;"));
	if (m_latestBandwidth > 0.0) {
		recommendation->setText(tr("当前链路估计带宽约为 %1，若出现丢帧可尝试降低采集帧率或缩小分辨率。")
			.arg(Utils::bytesHumanReadable(m_latestBandwidth)));
	} else {
		recommendation->setText(tr("当数据流稳定后，这里会给出链路带宽参考。"));
	}
	layout->addWidget(recommendation);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
	buttons->button(QDialogButtonBox::Ok)->setText(tr("应用"));
	buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);

	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	if (fpsSpin->isEnabled() && frameRateFeature && frameRateWritable) {
		const double target = fpsSpin->value();
		if (frameRateFeature->SetValue(target) != VmbErrorSuccess) {
			QMessageBox::warning(this, tr("设置失败"), tr("写入帧率时出现错误"));
		} else {
			flashStatus(tr("采集帧率已调整为 %1 FPS").arg(QString::number(target, 'f', 2)), 2800);
		}
	}

	if (enableCheck->isEnabled() && enableFeature && enableWritable) {
		const bool desired = enableCheck->isChecked();
		if (enableFeature->SetValue(desired) != VmbErrorSuccess) {
			QMessageBox::warning(this, tr("设置失败"), tr("无法更新帧率启用状态"));
		}
	}

	pollCameraStatus();
}

void CameraWindow::showAboutDialog() {
	QDialog dialog(this);
	dialog.setWindowTitle(tr("关于 Alvium Vision Studio"));
	dialog.setModal(true);
	dialog.setMinimumWidth(420);

	auto* layout = new QVBoxLayout(&dialog);
	layout->setContentsMargins(24, 24, 24, 24);
	layout->setSpacing(14);

	const QString version = QCoreApplication::applicationVersion().isEmpty()
		? tr("开发版")
		: QCoreApplication::applicationVersion();

	auto* heading = new QLabel(tr("<h2 style='color:#d9e6ff; margin:0;'>Alvium Vision Studio</h2>"), &dialog);
	heading->setTextFormat(Qt::RichText);
	heading->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	layout->addWidget(heading);

	auto* versionLabel = new QLabel(tr("版本：%1").arg(version), &dialog);
	versionLabel->setStyleSheet(QStringLiteral("color:#9fb8df;"));
	layout->addWidget(versionLabel);

	auto* description = new QLabel(tr("基于 Qt 6 与 Vimba X SDK 的工业相机控制台，提供实时预览、参数调节与状态监测。"), &dialog);
	description->setWordWrap(true);
	layout->addWidget(description);

	auto* author = new QLabel(tr("作者：CharmingZh"), &dialog);
	author->setStyleSheet(QStringLiteral("color:#b4c7eb;"));
	layout->addWidget(author);

	auto* contact = new QLabel(tr("联系方式：<a href='mailto:hello@charmingzh.dev'>hello@charmingzh.dev</a>"), &dialog);
	contact->setTextFormat(Qt::RichText);
	contact->setTextInteractionFlags(Qt::TextBrowserInteraction);
	contact->setOpenExternalLinks(true);
	contact->setStyleSheet(QStringLiteral("color:#9fc5ff;"));
	layout->addWidget(contact);

	auto* homepage = new QLabel(tr("个人主页：<a href='https://charmingzh.dev'>https://charmingzh.dev</a>"), &dialog);
	homepage->setTextFormat(Qt::RichText);
	homepage->setTextInteractionFlags(Qt::TextBrowserInteraction);
	homepage->setOpenExternalLinks(true);
	homepage->setStyleSheet(QStringLiteral("color:#9fc5ff;"));
	layout->addWidget(homepage);

	auto* featureList = new QLabel(tr("• 仪表盘实时展示连接、帧率、曝光等关键指标\n"
		"• 参数面板按类别分组，快速定位所需调节项\n"
		"• 自带帧率助手、快照与刷新工具"), &dialog);
	featureList->setTextFormat(Qt::PlainText);
	featureList->setWordWrap(true);
	featureList->setStyleSheet(QStringLiteral("color:#c8d9f3;"));
	layout->addWidget(featureList);

	auto* credits = new QLabel(tr("依赖组件：Qt 6 Widgets、Allied Vision Vimba X SDK"), &dialog);
	credits->setStyleSheet(QStringLiteral("color:#879dc6;"));
	layout->addWidget(credits);

	auto* footer = new QLabel(tr("© %1 Vision Lab · 用于内部演示与调试").arg(QDate::currentDate().year()), &dialog);
	footer->setStyleSheet(QStringLiteral("color:#7c92ba;"));
	layout->addWidget(footer);

	auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, &dialog);
	buttonBox->button(QDialogButtonBox::Close)->setText(tr("关闭"));
	connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	layout->addWidget(buttonBox);

	dialog.exec();
}

void CameraWindow::evaluateFocusMetrics(const QImage& frame) {
	if (!m_focusPanel || frame.isNull()) {
		return;
	}

	if (!m_focusTimer.isValid() || m_focusTimer.elapsed() >= 140) {
		m_focusTimer.restart();
	} else {
		return;
	}

	QRect roi = m_lastImageRoi;
	if (roi.isNull()) {
		roi = QRect(QPoint(0, 0), frame.size());
	}

	if (m_focusWatcher.isRunning()) {
		m_focusPendingFrame = frame;
		m_focusPendingRoi = roi;
		m_focusJobPending = true;
		return;
	}

	scheduleFocusEvaluation(frame, roi);
}

void CameraWindow::scheduleFocusEvaluation(const QImage& frame, const QRect& roi) {
	QRect frameRect(QPoint(0, 0), frame.size());
	QRect effectiveRoi = roi.isNull() ? frameRect : roi.intersected(frameRect);
	if (effectiveRoi.width() < 8 || effectiveRoi.height() < 8) {
		effectiveRoi = frameRect;
	}

	QImage workingImage;
	QRect evalRoi;
	if (effectiveRoi == frameRect) {
		workingImage = frame;
		evalRoi = effectiveRoi;
	} else {
		workingImage = frame.copy(effectiveRoi);
		evalRoi = QRect(QPoint(0, 0), workingImage.size());
	}

	m_focusPendingFrame = {};
	m_focusPendingRoi = {};
	m_focusJobPending = false;

	auto future = QtConcurrent::run([workingImage, evalRoi]() -> FocusEvaluator::Metrics {
		if (workingImage.isNull()) {
			return {};
		}
		QImage grayscale = workingImage.format() == QImage::Format_Grayscale8
			? workingImage
			: workingImage.convertToFormat(QImage::Format_Grayscale8);
		FocusEvaluator evaluator;
		return evaluator.evaluate(grayscale, evalRoi);
	});

	m_focusWatcher.setFuture(future);
}

void CameraWindow::handleFocusMetricsReady() {
	const FocusEvaluator::Metrics metrics = m_focusWatcher.result();
	if (metrics.valid && m_focusPanel) {
		m_focusMetrics = metrics;
		m_focusPanel->setMetrics(metrics);
	}

	if (m_focusJobPending && !m_focusPendingFrame.isNull()) {
		const QImage nextFrame = m_focusPendingFrame;
		const QRect nextRoi = m_focusPendingRoi;
		m_focusPendingFrame = {};
		m_focusPendingRoi = {};
		m_focusJobPending = false;
		scheduleFocusEvaluation(nextFrame, nextRoi);
	}
}

QRect CameraWindow::mapViewRectToImage(const QRect& viewRect, const QImage& frame) const {
	if (!m_view || frame.isNull() || viewRect.isNull()) {
		return {};
	}

	const QSize viewSize = m_view->size();
	if (viewSize.width() <= 0 || viewSize.height() <= 0) {
		return {};
	}

	QSize scaledSize = frame.size();
	scaledSize.scale(viewSize, Qt::KeepAspectRatio);
	const int offsetX = (viewSize.width() - scaledSize.width()) / 2;
	const int offsetY = (viewSize.height() - scaledSize.height()) / 2;
	QRect imageRect(QPoint(offsetX, offsetY), scaledSize);
	QRect clipped = viewRect.intersected(imageRect);
	if (clipped.width() < 4 || clipped.height() < 4) {
		return {};
	}

	const double scaleX = static_cast<double>(frame.width()) / static_cast<double>(imageRect.width());
	const double scaleY = static_cast<double>(frame.height()) / static_cast<double>(imageRect.height());
	int x = static_cast<int>(std::round((clipped.left() - offsetX) * scaleX));
	int y = static_cast<int>(std::round((clipped.top() - offsetY) * scaleY));
	int w = static_cast<int>(std::round(clipped.width() * scaleX));
	int h = static_cast<int>(std::round(clipped.height() * scaleY));

	x = std::clamp(x, 0, frame.width() - 1);
	y = std::clamp(y, 0, frame.height() - 1);
	w = std::clamp(w, 0, frame.width() - x);
	h = std::clamp(h, 0, frame.height() - y);

	QRect mapped(x, y, w, h);
	if (mapped.width() < 8 || mapped.height() < 8) {
		return {};
	}
	return mapped;
}

void CameraWindow::resetFocusPanel() {
	m_focusTimer.invalidate();
	m_focusMetrics = FocusEvaluator::Metrics{};
	m_lastImageRoi = {};
	m_lastViewRoi = {};
	if (m_focusPanel) {
		m_focusPanel->resetPanel();
	}
}

void CameraWindow::onOpen() {
	QString targetId;
	if (m_cameraCombo && m_cameraCombo->isEnabled() && m_cameraCombo->count() > 0) {
		targetId = m_cameraCombo->currentText();
	} else if (m_cam) {
		const auto cams = m_cam->listCameras();
		if (!cams.isEmpty()) {
			targetId = cams.first();
		}
	}

	if (targetId.isEmpty() || targetId == tr("未检测到相机")) {
		QMessageBox::information(this, tr("提示"), tr("当前没有可用的相机"));
		return;
	}

	if (!m_cam->open(targetId)) {
		QMessageBox::critical(this, tr("错误"), tr("相机打开失败"));
		return;
	}

	updateActionStates();
	pollCameraStatus();
}

void CameraWindow::onClose() {
	m_cam->close();
	m_streaming = false;
	m_latestFps = 0.0;
	m_latestBandwidth = 0.0;
	updateActionStates();
	updateConnectionBanner(false);
	Q_EMIT streamingStateChanged(false);
	pollCameraStatus();
	if (m_status) {
		m_status->setText(tr("FPS: --  |  带宽: --"));
	}
	resetFocusPanel();
}

void CameraWindow::onStart() {
	if (!m_cam->start()) {
		QMessageBox::critical(this, tr("错误"), tr("取流启动失败"));
		return;
	}
	m_streaming = true;
	m_focusTimer.restart();
	updateActionStates();
	flashStatus(tr("取流已开始"), 2500);
	if (m_statsBadge) {
		m_statsBadge->setText(tr("准备取流…"));
	}
	pollCameraStatus();
	Q_EMIT streamingStateChanged(true);
}

void CameraWindow::onStop() {
	m_cam->stop();
	m_focusTimer.invalidate();
	if (m_streaming) {
		flashStatus(tr("取流已停止"), 2500);
	}
	m_streaming = false;
	m_latestFps = 0.0;
	m_latestBandwidth = 0.0;
	updateActionStates();
	if (m_statsBadge) {
		m_statsBadge->setText(tr("已暂停"));
	}
	pollCameraStatus();
	if (m_status) {
		m_status->setText(tr("FPS: --  |  带宽: --"));
	}
	Q_EMIT streamingStateChanged(false);
}

void CameraWindow::onSnap()
{
	if (!m_cam->camera() && m_lastFrame.isNull()) {
		QMessageBox::information(this, tr("提示"), tr("请先连接相机并显示图像"));
		return;
	}

	QImage frame = m_lastFrame;
	if (frame.isNull() && m_view) {
		frame = m_view->grab().toImage();
	}
	if (frame.isNull()) {
		QMessageBox::warning(this, tr("保存失败"), tr("没有可保存的图像帧。"));
		return;
	}

	QString targetPath;
	if (m_snapshotPathProvider) {
		targetPath = m_snapshotPathProvider();
	}

	if (targetPath.isEmpty() && !m_snapshotDir.isEmpty()) {
		QDir dir(m_snapshotDir);
		if (!dir.exists()) {
			dir.mkpath(QStringLiteral("."));
		}
		const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmsszzz"));
		const QString baseName = m_snapshotPrefix.isEmpty() ? QStringLiteral("snap") : m_snapshotPrefix;
		QString candidate = dir.filePath(QStringLiteral("%1_%2.png").arg(baseName, timestamp));
		int guard = 1;
		while (QFileInfo::exists(candidate) && guard < 1000) {
			candidate = dir.filePath(QStringLiteral("%1_%2_%3.png").arg(baseName, timestamp).arg(++guard));
		}
		targetPath = candidate;
	}

	if (!targetPath.isEmpty()) {
		if (!frame.save(targetPath)) {
			QMessageBox::warning(this, tr("保存失败"), tr("无法写入 %1").arg(targetPath));
			return;
		}
		flashStatus(tr("快照已保存至 %1").arg(targetPath), 3000);
		Q_EMIT snapshotCaptured(targetPath);
		return;
	}

	const QString filePath = QFileDialog::getSaveFileName(
		this,
		tr("保存快照"),
		QDir::currentPath() + QStringLiteral("/snap.png"),
		tr("PNG 图像 (*.png)"));

	if (filePath.isEmpty()) {
		return;
	}

	if (!frame.save(filePath)) {
		QMessageBox::warning(this, tr("保存失败"), tr("无法写入 %1").arg(filePath));
		return;
	}

	flashStatus(tr("快照已保存至 %1").arg(filePath), 3000);
	Q_EMIT snapshotCaptured(filePath);
}

void CameraWindow::onRefreshCameras() {
	reloadCameraList();
	flashStatus(tr("相机列表已刷新"), 2000);
}

void CameraWindow::onRoiChanged(const QRect& roi) {
	m_lastViewRoi = roi;
	if (!m_lastFrame.isNull()) {
		m_lastImageRoi = mapViewRectToImage(roi, m_lastFrame);
		if (m_focusPanel) {
			m_focusPanel->setRoiInfo(m_lastFrame.size(), m_lastImageRoi);
		}
		evaluateFocusMetrics(m_lastFrame);
	}
	m_focusTimer.invalidate();
}

void CameraWindow::onFrame(const QImage& img)
{
	m_lastFrame = img;
	if (m_view) {
		m_view->setImage(img);
	}
	if (m_focusPanel) {
		if (m_lastViewRoi.isNull()) {
			m_lastImageRoi = QRect(QPoint(0, 0), img.size());
		} else {
			QRect mapped = mapViewRectToImage(m_lastViewRoi, img);
			m_lastImageRoi = mapped.isNull() ? QRect(QPoint(0, 0), img.size()) : mapped;
		}
		m_focusPanel->setRoiInfo(img.size(), m_lastImageRoi);
	}
	evaluateFocusMetrics(img);
	if (m_statsBadge && m_statsBadge->text().contains(tr("等待帧"))) {
		m_statsBadge->setText(tr("LIVE"));
	}
	Q_EMIT liveFrameReceived(img);
}

void CameraWindow::onStats(double fps, double bps) {
	const QString stats = tr("⚡ %1 FPS  ·  %2")
		.arg(QString::number(fps, 'f', 1))
		.arg(Utils::bytesHumanReadable(bps));

	if (m_statsBadge) {
		m_statsBadge->setText(stats);
	}
	if (m_status) {
		m_status->setText(stats);
	}

	m_latestFps = fps;
	m_latestBandwidth = bps;
	pollCameraStatus();
}
