#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QImage>
#include <QString>
#include <QVariantMap>
#include <QWidget>
#include <functional>

class QAction;
class QComboBox;
class QLabel;
class QTimer;
class QToolBar;
class QVBoxLayout;
class QFrame;
class QWidget;
class QSplitter;
class QShowEvent;

class VimbaController;
class ImageView;
class FeaturePanel;
class StatusDashboard;
class FocusSummaryPanel;

#include "camera/focus/FocusEvaluator.h"


class CameraWindow : public QWidget {
	Q_OBJECT
public:
	explicit CameraWindow(QWidget* parent = nullptr);
	~CameraWindow() override;

	void setSnapshotDirectory(const QString& directory);
	QString snapshotDirectory() const { return m_snapshotDir; }
	void setSnapshotNamingPrefix(const QString& prefix);
	void setSnapshotPathProvider(const std::function<QString()>& provider);
	ImageView* liveView() const { return m_view; }
	FeaturePanel* featurePanel() const { return m_panel; }
	StatusDashboard* statusDashboard() const { return m_dashboard; }
	bool isCameraConnected() const { return m_connected; }
	bool isStreaming() const { return m_streaming; }
	QVariantMap currentSnapshotMetrics() const;
    void setEmbeddedMode(bool embedded);
    bool isEmbeddedMode() const { return m_embeddedMode; }
	QComboBox* cameraSelector() const { return m_cameraCombo; }


public Q_SLOTS:
	void triggerSnapshot();
	void connectSelectedCamera();
	void disconnectCamera();
	void startStreaming();
	void stopStreaming();
	void refreshCameraList();

Q_SIGNALS:
	void liveFrameReceived(const QImage& image);
	void snapshotCaptured(const QString& filePath);
	void connectionStateChanged(bool connected, const QString& id, const QString& model);
	void streamingStateChanged(bool streaming);
	void tuningTimelineRequested();

private Q_SLOTS:
	void onOpen();
	void onClose();
	void onStart();
	void onStop();
	void onSnap();
	void onRefreshCameras();
	void onRoiChanged(const QRect& roi);

	void onFrame(const QImage& img);
	void onStats(double fps, double bps);

private:
	void buildUi();
	void reloadCameraList();
	void updateConnectionBanner(bool connected, const QString& id = {}, const QString& model = {});
	void updateActionStates();
	void flashStatus(const QString& message, int timeoutMs = 3000);
	void refreshDashboard();
	void pollCameraStatus();
	QString readFeatureValue(const char* name) const;
	QString readEnumDisplay(const char* name) const;
	QString readBoolDisplay(const char* name, const QString& trueText, const QString& falseText) const;
	void showFrameRateAssistant();
	void showAboutDialog();
	void evaluateFocusMetrics(const QImage& frame);
	void scheduleFocusEvaluation(const QImage& frame, const QRect& roi);
	void handleFocusMetricsReady();
	QRect mapViewRectToImage(const QRect& viewRect, const QImage& frame) const;
	void resetFocusPanel();
    void applyEmbeddedMode();
	void applySplitterPreset();
	void showEvent(QShowEvent* event) override;
    QString resolveConfigDirectory() const;

	VimbaController* m_cam{nullptr};
	ImageView* m_view{nullptr};
	FeaturePanel* m_panel{nullptr};
	FocusSummaryPanel* m_focusPanel{nullptr};
	QComboBox* m_cameraCombo{nullptr};
	QLabel* m_status{nullptr};
	QLabel* m_statusFlash{nullptr};
	QLabel* m_cameraBadge{nullptr};
	QLabel* m_statsBadge{nullptr};
	QAction* m_actOpen{nullptr};
	QAction* m_actAbout{nullptr};
	QAction* m_actFrameAssist{nullptr};
	QAction* m_actClose{nullptr};
	QAction* m_actStart{nullptr};
	QAction* m_actStop{nullptr};
	QAction* m_actSnap{nullptr};
	QAction* m_actTuningTimeline{nullptr};
	bool m_streaming{false};
	StatusDashboard* m_dashboard{nullptr};
	QTimer* m_statusTimer{nullptr};
	QTimer* m_flashTimer{nullptr};
	QString m_lastCameraId;
	QString m_lastCameraModel;
	QVariantMap m_cachedMetrics;
	double m_latestFps{0.0};
	double m_latestBandwidth{0.0};
	QString m_snapshotDir;
	QString m_snapshotPrefix{QStringLiteral("snap")};
	std::function<QString()> m_snapshotPathProvider;
	QImage m_lastFrame;
	bool m_connected{false};
	FocusEvaluator::Metrics m_focusMetrics;
	QElapsedTimer m_focusTimer;
	QRect m_lastViewRoi;
	QRect m_lastImageRoi;
	QFutureWatcher<FocusEvaluator::Metrics> m_focusWatcher;
	QImage m_focusPendingFrame;
	QRect m_focusPendingRoi;
	bool m_focusJobPending{false};
	bool m_embeddedMode{false};
	QToolBar* m_primaryToolbar{nullptr};
	QVBoxLayout* m_rootLayout{nullptr};
	QVBoxLayout* m_mainLayout{nullptr};
	QWidget* m_headerPanel{nullptr};
	QLabel* m_metaLabel{nullptr};
	QLabel* m_hintLabel{nullptr};
	QLabel* m_checklistLabel{nullptr};
	QFrame* m_statusFrame{nullptr};
	QWidget* m_inspectorPane{nullptr};
	QFrame* m_featureCard{nullptr};
    QSplitter* m_splitter{nullptr};
    bool m_splitterPresetApplied{false};
};
