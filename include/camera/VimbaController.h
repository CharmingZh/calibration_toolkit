#pragma once
#include <QObject>
#include <QThread>
#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QStringList>
#include <optional>
#include <memory>


// Vimba X C++ API
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:5054)
#endif
#include <VmbCPP/VmbCPP.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif


class VimbaController : public QObject {
	Q_OBJECT
public:
	explicit VimbaController(QObject* parent = nullptr);
	~VimbaController();

// 设备
QStringList listCameras();
bool open(const QString& idOrIndex);
void close();

// 取流控制
bool start();
void stop();

// 通用 GenICam 特性访问
VmbCPP::FeaturePtr feature(const char* name);
std::vector<VmbCPP::FeaturePtr> allFeatures();

bool applyConfigurationProfile(const QString& directory, const QString& cameraId, QString* statusMessage = nullptr);

VmbCPP::CameraPtr camera() const { return m_cam; }

	void processFrame(const VmbCPP::FramePtr& frame);

Q_SIGNALS:
	void frameReady(const QImage& img);
	void statsUpdated(double fps, double bandwidthBps);
	void cameraOpened(const QString& id, const QString& model);
	void cameraClosed();
	void errorOccured(const QString& msg);

private:
	VmbCPP::VmbSystem& m_sys;
	VmbCPP::CameraPtr m_cam;
	VmbCPP::FramePtrVector m_frames;
	VmbCPP::IFrameObserverPtr m_observer;

	QAtomicInteger<bool> m_running{false};
	QElapsedTimer m_fpsTimer;
	int64_t m_frameCount = 0;
	int64_t m_bytesAccum = 0;
	std::optional<VmbPixelFormatType> m_lastUnsupportedPixelFormat;
};
