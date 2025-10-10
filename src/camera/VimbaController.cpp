#include "camera/VimbaController.h"
#include "camera/Utils.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QLocale>
#include <QXmlStreamReader>
#include <QStringList>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

using namespace VmbCPP;

namespace {

QString describeCamera(const CameraPtr& cam) {
    if (!cam) {
        return {};
    }
    std::string id;
    std::string model;
    cam->GetID(id);
    cam->GetModel(model);
    if (!id.empty() && !model.empty()) {
        return QString::fromLocal8Bit(id.c_str()) + QStringLiteral(" — ") + QString::fromLocal8Bit(model.c_str());
    }
    if (!id.empty()) {
        return QString::fromLocal8Bit(id.c_str());
    }
    if (!model.empty()) {
        return QString::fromLocal8Bit(model.c_str());
    }
    return QStringLiteral("Unknown");
}

QString errorToQString(VmbErrorType err) {
    return QStringLiteral("Vimba error (%1)").arg(static_cast<int>(err));
}

QString pixelFormatDisplayName(VmbPixelFormatType format) {
    switch (format) {
    case VmbPixelFormatMono8:
        return QStringLiteral("Mono8");
    case VmbPixelFormatMono16:
        return QStringLiteral("Mono16");
    case VmbPixelFormatRgb8:
        return QStringLiteral("RGB8");
    case VmbPixelFormatBgr8:
        return QStringLiteral("BGR8");
    case VmbPixelFormatBayerRG8:
        return QStringLiteral("BayerRG8");
    case VmbPixelFormatBayerBG8:
        return QStringLiteral("BayerBG8");
    case VmbPixelFormatBayerGR8:
        return QStringLiteral("BayerGR8");
    case VmbPixelFormatBayerGB8:
        return QStringLiteral("BayerGB8");
    default:
        return QStringLiteral("0x%1").arg(QString::number(static_cast<qulonglong>(format), 16).toUpper());
    }
}

int expectedBytesPerPixel(VmbPixelFormatType format) {
    switch (format) {
    case VmbPixelFormatMono8:
    case VmbPixelFormatBayerRG8:
    case VmbPixelFormatBayerBG8:
    case VmbPixelFormatBayerGR8:
    case VmbPixelFormatBayerGB8:
        return 1;
    case VmbPixelFormatRgb8:
    case VmbPixelFormatBgr8:
        return 3;
    case VmbPixelFormatMono16:
        return 2;
    default:
        return 0;
    }
}

int computeStride(int width, int height, int bufferSize, VmbPixelFormatType format) {
    const int bytesPerPixel = expectedBytesPerPixel(format);
    if (bytesPerPixel <= 0) {
        return 0;
    }
    const int baseline = width * bytesPerPixel;
    if (height <= 0 || bufferSize <= 0) {
        return baseline;
    }
    const int derived = bufferSize / std::max(1, height);
    if (derived >= baseline) {
        return derived;
    }
    return baseline;
}

QImage convertFrameToImage(int width,
                           int height,
                           VmbPixelFormatType format,
                           const VmbUchar_t* data,
                           int bufferSize,
                           QString* errorMessage) {
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!data || width <= 0 || height <= 0) {
        return {};
    }

    const int stride = computeStride(width, height, bufferSize, format);
    switch (format) {
    case VmbPixelFormatMono8:
        return Utils::makeImageFromMono8(reinterpret_cast<const uint8_t*>(data), width, height, stride);
    case VmbPixelFormatRgb8: {
        cv::Mat rgb(height, width, CV_8UC3, const_cast<VmbUchar_t*>(data), stride);
        return QImage(rgb.data, rgb.cols, rgb.rows, rgb.step, QImage::Format_RGB888).copy();
    }
    case VmbPixelFormatBgr8: {
        cv::Mat bgr(height, width, CV_8UC3, const_cast<VmbUchar_t*>(data), stride);
        cv::Mat rgb;
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
        return QImage(rgb.data, rgb.cols, rgb.rows, rgb.step, QImage::Format_RGB888).copy();
    }
    case VmbPixelFormatMono16: {
        cv::Mat mono16(height, width, CV_16UC1, const_cast<VmbUchar_t*>(data), stride);
        cv::Mat normalized;
        mono16.convertTo(normalized, CV_8U, 1.0 / 256.0);
        return QImage(normalized.data, normalized.cols, normalized.rows, normalized.step, QImage::Format_Grayscale8).copy();
    }
    case VmbPixelFormatBayerRG8:
    case VmbPixelFormatBayerBG8:
    case VmbPixelFormatBayerGR8:
    case VmbPixelFormatBayerGB8: {
        cv::Mat raw(height, width, CV_8UC1, const_cast<VmbUchar_t*>(data), stride);
        cv::Mat rgb;
        int code = 0;
        switch (format) {
        case VmbPixelFormatBayerRG8:
            code = cv::COLOR_BayerRG2RGB;
            break;
        case VmbPixelFormatBayerBG8:
            code = cv::COLOR_BayerBG2RGB;
            break;
        case VmbPixelFormatBayerGR8:
            code = cv::COLOR_BayerGR2RGB;
            break;
        case VmbPixelFormatBayerGB8:
            code = cv::COLOR_BayerGB2RGB;
            break;
        default:
            break;
        }
        if (code == 0) {
            break;
        }
        cv::cvtColor(raw, rgb, code);
        return QImage(rgb.data, rgb.cols, rgb.rows, rgb.step, QImage::Format_RGB888).copy();
    }
    default:
        break;
    }

    if (errorMessage) {
        *errorMessage = QObject::tr("当前像素格式 (%1) 暂未支持，请在 PixelFormat 中切换到 Mono8 或 Bayer RG8。").arg(pixelFormatDisplayName(format));
    }
    return {};
}

class ControllerFrameObserver : public IFrameObserver {
public:
    ControllerFrameObserver(const CameraPtr& cam, VimbaController* controller)
        : IFrameObserver(cam)
        , m_controller(controller) {}

    void FrameReceived(const FramePtr frame) override {
        if (!m_controller) {
            return;
        }
        QMetaObject::invokeMethod(
            m_controller,
            [ctrl = m_controller, frame]() { ctrl->processFrame(frame); },
            Qt::QueuedConnection);
    }

private:
    VimbaController* m_controller{nullptr};
};

CameraPtr cameraById(const CameraPtrVector& cameras, const QString& id) {
    for (const auto& cam : cameras) {
        if (!cam) {
            continue;
        }
        std::string rawId;
        cam->GetID(rawId);
        if (QString::fromLocal8Bit(rawId.c_str()).compare(id, Qt::CaseInsensitive) == 0) {
            return cam;
        }
    }
    return CameraPtr();
}

FeaturePtr resolveFeature(VimbaController* controller, const QString& name) {
    if (!controller || name.isEmpty()) {
        return FeaturePtr();
    }
    const QByteArray utf8 = name.toUtf8();
    return controller->feature(utf8.constData());
}

bool setFeatureValueFromString(const FeaturePtr& feature,
                               const QString& rawValue,
                               const QString& featureName,
                               QStringList& warnings) {
    if (!feature) {
        warnings << QObject::tr("特性 %1 未找到，已跳过").arg(featureName);
        return false;
    }

    VmbFeatureDataType dataType = VmbFeatureDataUnknown;
    if (feature->GetDataType(dataType) != VmbErrorSuccess) {
        warnings << QObject::tr("无法确定特性 %1 的数据类型").arg(featureName);
        return false;
    }

    bool writable = false;
    feature->IsWritable(writable);

    auto addFailedWarning = [&](const QString& reason) {
        warnings << QObject::tr("写入 %1 失败：%2").arg(featureName, reason);
    };

    VmbErrorType err = VmbErrorSuccess;
    switch (dataType) {
    case VmbFeatureDataInt: {
        bool ok = false;
        const qint64 desired = QLocale::c().toLongLong(rawValue, &ok);
        if (!ok) {
            addFailedWarning(QObject::tr("值 %1 无法转换为整数").arg(rawValue));
            return false;
        }
        if (!writable) {
            VmbInt64_t current = 0;
            if (feature->GetValue(current) == VmbErrorSuccess && current == desired) {
                return true;
            }
            addFailedWarning(QObject::tr("特性为只读"));
            return false;
        }
        err = feature->SetValue(static_cast<VmbInt64_t>(desired));
        break;
    }
    case VmbFeatureDataFloat: {
        bool ok = false;
        const double desired = QLocale::c().toDouble(rawValue, &ok);
        if (!ok) {
            addFailedWarning(QObject::tr("值 %1 无法转换为浮点数").arg(rawValue));
            return false;
        }
        if (!writable) {
            double current = 0.0;
            if (feature->GetValue(current) == VmbErrorSuccess && std::abs(current - desired) < 1e-6) {
                return true;
            }
            addFailedWarning(QObject::tr("特性为只读"));
            return false;
        }
        err = feature->SetValue(desired);
        break;
    }
    case VmbFeatureDataBool: {
        const QString normalized = rawValue.trimmed().toLower();
        bool desired = false;
        if (normalized == QStringLiteral("1") || normalized == QStringLiteral("true") || normalized == QStringLiteral("on") || normalized == QStringLiteral("yes")) {
            desired = true;
        } else if (normalized == QStringLiteral("0") || normalized == QStringLiteral("false") || normalized == QStringLiteral("off") || normalized == QStringLiteral("no")) {
            desired = false;
        } else {
            bool ok = false;
            const int asInt = QLocale::c().toInt(rawValue, &ok);
            if (ok) {
                desired = asInt != 0;
            } else {
                addFailedWarning(QObject::tr("值 %1 无法转换为布尔").arg(rawValue));
                return false;
            }
        }
        if (!writable) {
            bool current = false;
            if (feature->GetValue(current) == VmbErrorSuccess && current == desired) {
                return true;
            }
            addFailedWarning(QObject::tr("特性为只读"));
            return false;
        }
        err = feature->SetValue(desired);
        break;
    }
    case VmbFeatureDataEnum: {
        std::string desired = rawValue.toStdString();
        if (!writable) {
            std::string current;
            if (feature->GetValue(current) == VmbErrorSuccess && QString::fromStdString(current).compare(rawValue, Qt::CaseInsensitive) == 0) {
                return true;
            }
            addFailedWarning(QObject::tr("特性为只读"));
            return false;
        }
        err = feature->SetValue(desired.c_str());
        break;
    }
    case VmbFeatureDataString: {
        std::string desired = rawValue.toStdString();
        if (!writable) {
            std::string current;
            if (feature->GetValue(current) == VmbErrorSuccess && QString::fromStdString(current) == rawValue) {
                return true;
            }
            addFailedWarning(QObject::tr("特性为只读"));
            return false;
        }
        err = feature->SetValue(desired.c_str());
        break;
    }
    case VmbFeatureDataCommand: {
        if (!writable) {
            return false;
        }
        const QString normalized = rawValue.trimmed().toLower();
        if (normalized == QStringLiteral("1") || normalized == QStringLiteral("true") || normalized == QStringLiteral("run") || normalized == QStringLiteral("execute")) {
            err = feature->RunCommand();
        }
        break;
    }
    default:
        addFailedWarning(QObject::tr("不支持的数据类型"));
        return false;
    }

    if (err != VmbErrorSuccess) {
        addFailedWarning(errorToQString(err));
        return false;
    }
    return true;
}

void applyFeatureElement(QXmlStreamReader& xml,
                         VimbaController* controller,
                         QStringList& warnings) {
    const QString featureName = xml.attributes().value(QStringLiteral("Name")).toString();
    const QString featureValue = xml.attributes().value(QStringLiteral("Value")).toString();
    if (!featureName.isEmpty()) {
        const FeaturePtr feature = resolveFeature(controller, featureName);
        setFeatureValueFromString(feature, featureValue, featureName, warnings);
    }
    xml.skipCurrentElement();
}

void applySelectorGroup(QXmlStreamReader& xml,
                        VimbaController* controller,
                        QStringList& warnings) {
    const QString selectorName = xml.attributes().value(QStringLiteral("Name")).toString();
    const QString selectorValue = xml.attributes().value(QStringLiteral("Value")).toString();
    if (!selectorName.isEmpty()) {
        const FeaturePtr selectorFeature = resolveFeature(controller, selectorName);
        setFeatureValueFromString(selectorFeature, selectorValue, selectorName, warnings);
    }

    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("Feature")) {
            applyFeatureElement(xml, controller, warnings);
        } else if (xml.name() == QLatin1String("SelectorGroup")) {
            applySelectorGroup(xml, controller, warnings);
        } else {
            xml.skipCurrentElement();
        }
    }
}

void applyCameraInfo(QXmlStreamReader& xml,
                     VimbaController* controller,
                     QStringList& warnings) {
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("Feature")) {
            applyFeatureElement(xml, controller, warnings);
        } else if (xml.name() == QLatin1String("SelectorGroup")) {
            applySelectorGroup(xml, controller, warnings);
        } else {
            xml.skipCurrentElement();
        }
    }
}

} // namespace


VimbaController::VimbaController(QObject* parent)
    : QObject(parent)
    , m_sys(VmbCPP::VmbSystem::GetInstance()) {
    VmbErrorType err = m_sys.Startup();
    if (err != VmbErrorSuccess) {
        Q_EMIT errorOccured(QStringLiteral("Vimba startup failed: %1").arg(errorToQString(err)));
    }
}


VimbaController::~VimbaController() {
    stop();
    close();
    VmbErrorType err = m_sys.Shutdown();
    if (err != VmbErrorSuccess) {
        qWarning() << "Vimba shutdown returned" << static_cast<int>(err);
    }
}


QStringList VimbaController::listCameras() {
    QStringList out;
    CameraPtrVector cameras;
    if (m_sys.GetCameras(cameras) != VmbErrorSuccess) {
        return out;
    }
    for (const auto& cam : cameras) {
        if (cam) {
            std::string id;
            if (cam->GetID(id) == VmbErrorSuccess && !id.empty()) {
                out << QString::fromStdString(id);
                continue;
            }
        }
        out << describeCamera(cam);
    }
    return out;
}


bool VimbaController::open(const QString& idOrIndex) {
    close();

    CameraPtrVector cameras;
    if (m_sys.GetCameras(cameras) != VmbErrorSuccess) {
        Q_EMIT errorOccured(QStringLiteral("无法获取相机列表"));
        return false;
    }

    CameraPtr candidate;
    bool okIndex = false;
    const int index = idOrIndex.toInt(&okIndex);
    if (okIndex && index >= 0 && index < static_cast<int>(cameras.size())) {
        candidate = cameras[static_cast<size_t>(index)];
    }
    if (!candidate) {
        candidate = cameraById(cameras, idOrIndex);
    }

    VmbErrorType err = VmbErrorSuccess;
    if (candidate) {
        err = candidate->Open(VmbAccessModeFull);
        if (err == VmbErrorSuccess) {
            m_cam = candidate;
        }
    } else {
        CameraPtr opened;
        err = m_sys.OpenCameraByID(idOrIndex.toLocal8Bit().constData(), VmbAccessModeFull, opened);
        if (err == VmbErrorSuccess) {
            m_cam = opened;
        }
    }

    if (err != VmbErrorSuccess || !m_cam) {
        Q_EMIT errorOccured(QStringLiteral("打开相机失败: %1").arg(errorToQString(err)));
        return false;
    }

    m_observer = IFrameObserverPtr(new ControllerFrameObserver(m_cam, this));

    std::string id;
    std::string model;
    m_cam->GetID(id);
    m_cam->GetModel(model);
    Q_EMIT cameraOpened(QString::fromLocal8Bit(id.c_str()), QString::fromLocal8Bit(model.c_str()));
    return true;
}


void VimbaController::close() {
    if (!m_cam) {
        return;
    }

    stop();

    try {
        m_observer.reset();
        m_cam->Close();
        Q_EMIT cameraClosed();
    } catch (...) {
        Q_EMIT errorOccured(QStringLiteral("关闭相机时发生异常"));
    }
    m_cam.reset();
}


bool VimbaController::start() {
    if (!m_cam) {
        Q_EMIT errorOccured(QStringLiteral("请先打开相机"));
        return false;
    }
    if (m_running.loadAcquire()) {
        return true;
    }

    try {
        m_frames.clear();

        if (FeaturePtr autoNegotiate = feature("StreamAutoNegotiatePacketSize")) {
            bool writable = false;
            autoNegotiate->IsWritable(writable);
            if (writable) {
                bool enabled = false;
                if (autoNegotiate->GetValue(enabled) != VmbErrorSuccess || !enabled) {
                    autoNegotiate->SetValue(true);
                }
            }
        }
        if (FeaturePtr adjustPacket = feature("GVSPAdjustPacketSize")) {
            adjustPacket->RunCommand();
        }

        const int kNumBuffers = 12;
        VmbUint32_t payload = 0;
        if (m_cam->GetPayloadSize(payload) != VmbErrorSuccess || payload == 0) {
            FeaturePtr payloadFeature = feature("PayloadSize");
            if (payloadFeature) {
                VmbInt64_t payloadValue = 0;
                if (payloadFeature->GetValue(payloadValue) == VmbErrorSuccess && payloadValue > 0) {
                    payload = static_cast<VmbUint32_t>(payloadValue);
                }
            }
        }
        if (payload == 0) {
            payload = 4 * 1024 * 1024;
        }
        for (int i = 0; i < kNumBuffers; ++i) {
            FramePtr frame(new Frame(static_cast<VmbInt64_t>(payload)));
            if (m_observer) {
                VmbErrorType obsErr = frame->RegisterObserver(m_observer);
                if (obsErr != VmbErrorSuccess) {
                    Q_EMIT errorOccured(QStringLiteral("注册帧观察者失败: %1").arg(errorToQString(obsErr)));
                    return false;
                }
            }
            VmbErrorType announceErr = m_cam->AnnounceFrame(frame);
            if (announceErr != VmbErrorSuccess) {
                Q_EMIT errorOccured(QStringLiteral("AnnounceFrame 失败: %1").arg(errorToQString(announceErr)));
                return false;
            }
            m_frames.push_back(frame);
        }

        VmbErrorType err = m_cam->StartCapture();
        if (err != VmbErrorSuccess) {
            Q_EMIT errorOccured(QStringLiteral("StartCapture 失败: %1").arg(errorToQString(err)));
            return false;
        }

        for (auto& frame : m_frames) {
            VmbErrorType queueErr = m_cam->QueueFrame(frame);
            if (queueErr != VmbErrorSuccess) {
                Q_EMIT errorOccured(QStringLiteral("QueueFrame 失败: %1").arg(errorToQString(queueErr)));
            }
        }

        FeaturePtr startAcq = feature("AcquisitionStart");
        if (startAcq) {
            startAcq->RunCommand();
        }

        m_running.storeRelease(true);
        m_fpsTimer.restart();
        m_frameCount = 0;
        m_bytesAccum = 0;
        return true;
    } catch (...) {
        Q_EMIT errorOccured(QStringLiteral("start() exception"));
        return false;
    }
}


void VimbaController::stop() {
    if (!m_cam) {
        return;
    }

    if (!m_running.loadAcquire()) {
        return;
    }
    m_running.storeRelease(false);

    try {
        FeaturePtr stopAcq = feature("AcquisitionStop");
        if (stopAcq) {
            stopAcq->RunCommand();
        }
        m_cam->EndCapture();
        m_cam->FlushQueue();
        m_cam->RevokeAllFrames();
        for (auto& frame : m_frames) {
            if (frame) {
                frame->UnregisterObserver();
            }
        }
        m_frames.clear();
    } catch (...) {
        Q_EMIT errorOccured(QStringLiteral("stop() exception"));
    }
}

bool VimbaController::applyConfigurationProfile(const QString& directory,
                                                const QString& cameraId,
                                                QString* statusMessage) {
    if (statusMessage) {
        statusMessage->clear();
    }

    if (!m_cam) {
        if (statusMessage) {
            *statusMessage = QObject::tr("请先连接相机");
        }
        return false;
    }

    QDir dir(directory);
    if (!dir.exists()) {
        if (statusMessage) {
            *statusMessage = QObject::tr("配置目录不存在：%1").arg(QDir::toNativeSeparators(directory));
        }
        return false;
    }

    QString effectiveCameraId = cameraId;
    if (effectiveCameraId.isEmpty()) {
        std::string currentId;
        if (m_cam->GetID(currentId) == VmbErrorSuccess) {
            effectiveCameraId = QString::fromLocal8Bit(currentId.c_str());
        }
    }

    const QStringList filters{QStringLiteral("*.xml"), QStringLiteral("*.XML")};
    const QFileInfoList files = dir.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::Name);
    if (files.isEmpty()) {
        if (statusMessage) {
            *statusMessage = QObject::tr("未在 %1 中找到配置文件").arg(dir.absolutePath());
        }
        return false;
    }

    for (const QFileInfo& info : files) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "无法读取配置文件" << info.absoluteFilePath();
            continue;
        }

        QXmlStreamReader xml(&file);
        bool matchedCamera = false;
        QStringList warnings;

        while (xml.readNextStartElement()) {
            if (xml.name() == QLatin1String("CameraInfo")) {
                const QString idAttr = xml.attributes().value(QStringLiteral("Id")).toString();
                if (effectiveCameraId.isEmpty() || idAttr.compare(effectiveCameraId, Qt::CaseInsensitive) == 0) {
                    matchedCamera = true;
                    applyCameraInfo(xml, this, warnings);
                    break;
                }
                xml.skipCurrentElement();
            } else {
                xml.skipCurrentElement();
            }
        }

        if (matchedCamera && !xml.hasError()) {
            if (!warnings.isEmpty()) {
                for (const QString& warning : std::as_const(warnings)) {
                    qWarning() << "配置写入警告:" << warning;
                }
            }
            if (statusMessage) {
                if (warnings.isEmpty()) {
                    *statusMessage = QObject::tr("已应用配置文件 %1").arg(info.fileName());
                } else {
                    *statusMessage = QObject::tr("已应用配置文件 %1（跳过 %2 项）")
                                         .arg(info.fileName())
                                         .arg(warnings.size());
                }
            }
            return true;
        }

        if (matchedCamera && xml.hasError()) {
            qWarning() << "配置文件解析失败" << info.absoluteFilePath() << ":" << xml.errorString();
        }
    }

    if (statusMessage && !files.isEmpty()) {
        *statusMessage = QObject::tr("未找到匹配当前相机的配置文件");
    }
    return false;
}


FeaturePtr VimbaController::feature(const char* name) {
    if (!m_cam || !name) {
        return FeaturePtr();
    }
    FeaturePtr f;
    m_cam->GetFeatureByName(name, f);
    return f;
}


std::vector<FeaturePtr> VimbaController::allFeatures() {
    std::vector<FeaturePtr> out;
    if (!m_cam) {
        return out;
    }
    FeaturePtrVector fs;
    if (m_cam->GetFeatures(fs) == VmbErrorSuccess) {
        out.assign(fs.begin(), fs.end());
    }
    return out;
}


void VimbaController::processFrame(const FramePtr& frame) {
    if (!frame) {
        return;
    }

    VmbFrameStatusType status = VmbFrameStatusInvalid;
    frame->GetReceiveStatus(status);

    QImage image;
    if (status == VmbFrameStatusComplete) {
        VmbUint32_t w = 0;
        VmbUint32_t h = 0;
        frame->GetWidth(w);
        frame->GetHeight(h);

        VmbPixelFormatType pf = VmbPixelFormatMono8;
        frame->GetPixelFormat(pf);

        VmbUint32_t size = 0;
        frame->GetBufferSize(size);
        const VmbUchar_t* data = nullptr;
        frame->GetImage(data);

        if (data != nullptr && w > 0 && h > 0) {
            QString conversionError;
            image = convertFrameToImage(static_cast<int>(w), static_cast<int>(h), pf, data, static_cast<int>(size), &conversionError);

            if (!image.isNull()) {
                ++m_frameCount;
                m_bytesAccum += size;
                const qint64 elapsedMs = m_fpsTimer.elapsed();
                if (elapsedMs > 300) {
                    const double seconds = static_cast<double>(elapsedMs) / 1000.0;
                    const double fps = seconds > 0.0 ? static_cast<double>(m_frameCount) / seconds : 0.0;
                    const double bps = seconds > 0.0 ? static_cast<double>(m_bytesAccum) / seconds : 0.0;
                    Q_EMIT statsUpdated(fps, bps);
                    m_fpsTimer.restart();
                    m_frameCount = 0;
                    m_bytesAccum = 0;
                }
                m_lastUnsupportedPixelFormat.reset();
            } else if (!conversionError.isEmpty()) {
                if (!m_lastUnsupportedPixelFormat.has_value() || m_lastUnsupportedPixelFormat.value() != pf) {
                    m_lastUnsupportedPixelFormat = pf;
                    Q_EMIT errorOccured(conversionError);
                }
            }
        }
    }

    if (m_running.loadAcquire() && m_cam) {
        m_cam->QueueFrame(frame);
    }

    if (!image.isNull()) {
        Q_EMIT frameReady(image);
    }
}
