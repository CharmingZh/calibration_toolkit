#include "ParameterDialog.h"

#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QStyle>
#include <QTextCursor>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>

#include <opencv2/core.hpp>

namespace mycalib {

namespace {
cv::Mat toDoubleMat(const cv::Mat &input)
{
    if (input.empty()) {
        return cv::Mat();
    }
    if (input.type() == CV_64F) {
        return input.clone();
    }
    cv::Mat converted;
    input.convertTo(converted, CV_64F);
    return converted;
}
} // namespace

ParameterDialog::ParameterDialog(const CalibrationOutput &output, QWidget *parent)
    : QDialog(parent)
    , m_output(output)
{
    setWindowTitle(tr("Calibration Parameters"));
    setModal(true);
    setMinimumSize(600, 520);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 20, 24, 24);
    mainLayout->setSpacing(12);

    auto *header = new QHBoxLayout();
    header->setSpacing(12);

    auto *formatLabel = new QLabel(tr("Format"), this);
    QFont formatFont = formatLabel->font();
    formatFont.setBold(true);
    formatLabel->setFont(formatFont);
    header->addWidget(formatLabel);

    m_styleCombo = new QComboBox(this);
    m_styleCombo->addItem(tr("Python (NumPy style)"), static_cast<int>(SnippetStyle::Python));
    m_styleCombo->addItem(tr("C++ (OpenCV style)"), static_cast<int>(SnippetStyle::Cpp));
    m_styleCombo->addItem(tr("Plain text summary"), static_cast<int>(SnippetStyle::PlainText));
    header->addWidget(m_styleCombo, 1);

    m_copyButton = new QPushButton(tr("Copy"), this);
    m_copyButton->setProperty("accent", true);
    header->addWidget(m_copyButton);

    mainLayout->addLayout(header);

    m_preview = new QPlainTextEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setWordWrapMode(QTextOption::NoWrap);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_preview->setMinimumHeight(360);
    mainLayout->addWidget(m_preview, 1);

    m_statusLabel = new QLabel(this);
    QFont statusFont = m_statusLabel->font();
    statusFont.setPointSizeF(statusFont.pointSizeF() - 1.0);
    m_statusLabel->setFont(statusFont);
    m_statusLabel->setStyleSheet(QStringLiteral("color: #4c7bff;"));
    mainLayout->addWidget(m_statusLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(tr("Close"));
    mainLayout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_styleCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_currentStyle = static_cast<SnippetStyle>(m_styleCombo->itemData(index).toInt());
        refreshPreview();
    });

    connect(m_copyButton, &QPushButton::clicked, this, [this]() {
        if (auto *clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(m_preview->toPlainText());
            if (m_statusLabel) {
                m_statusLabel->setText(tr("Copied to clipboard."));
                QTimer::singleShot(2000, m_statusLabel, &QLabel::clear);
            }
        }
    });

    m_styleCombo->setCurrentIndex(0);
    refreshPreview();
}

void ParameterDialog::refreshPreview()
{
    QString text;
    switch (m_currentStyle) {
    case SnippetStyle::Python:
        text = buildPythonSnippet();
        break;
    case SnippetStyle::Cpp:
        text = buildCppSnippet();
        break;
    case SnippetStyle::PlainText:
        text = buildPlainSnippet();
        break;
    }
    m_preview->setPlainText(text);
    m_preview->moveCursor(QTextCursor::Start);
}

QString ParameterDialog::buildPythonSnippet() const
{
    QString result;
    const cv::Mat camera = toDoubleMat(m_output.cameraMatrix);
    const cv::Mat dist = toDoubleMat(m_output.distCoeffs);

    result += QStringLiteral("# Intrinsic camera matrix\n");
    result += QStringLiteral("camera_matrix = [\n");
    if (!camera.empty()) {
        for (int r = 0; r < camera.rows; ++r) {
            QStringList row;
            for (int c = 0; c < camera.cols; ++c) {
                row << formatDouble(camera.at<double>(r, c));
            }
            result += QStringLiteral("    [%1]%2\n")
                          .arg(row.join(QStringLiteral(", ")))
                          .arg(r + 1 == camera.rows ? QString() : QStringLiteral(","));
        }
    }
    result += QStringLiteral("]\n\n");

    result += QStringLiteral("# Distortion coefficients (k1, k2, p1, p2, k3, ...)\n");
    if (!dist.empty()) {
        QStringList coeffs;
        const int count = static_cast<int>(dist.total());
        for (int i = 0; i < count; ++i) {
            coeffs << formatDouble(dist.at<double>(i));
        }
        result += QStringLiteral("dist_coeffs = [%1]\n\n").arg(coeffs.join(QStringLiteral(", ")));
    } else {
        result += QStringLiteral("dist_coeffs = []\n\n");
    }

    result += QStringLiteral("image_size = (%1, %2)\n\n")
                  .arg(m_output.imageSize.width)
                  .arg(m_output.imageSize.height);

    result += QStringLiteral("metrics = {\n");
    result += QStringLiteral("    \"rms_px\": %1,\n").arg(formatDouble(m_output.metrics.rms));
    result += QStringLiteral("    \"mean_px\": %1,\n").arg(formatDouble(m_output.metrics.meanErrorPx));
    result += QStringLiteral("    \"median_px\": %1,\n").arg(formatDouble(m_output.metrics.medianErrorPx));
    result += QStringLiteral("    \"p95_px\": %1,\n").arg(formatDouble(m_output.metrics.p95ErrorPx));
    result += QStringLiteral("    \"max_px\": %1\n").arg(formatDouble(m_output.metrics.maxErrorPx));
    result += QStringLiteral("}\n");

    return result;
}

QString ParameterDialog::buildCppSnippet() const
{
    QString result;
    const cv::Mat camera = toDoubleMat(m_output.cameraMatrix);
    const cv::Mat dist = toDoubleMat(m_output.distCoeffs);

    result += QStringLiteral("// Intrinsic calibration\n");
    if (!camera.empty()) {
        result += QStringLiteral("cv::Mat cameraMatrix = (cv::Mat_<double>(%1, %2) <<\n")
                      .arg(camera.rows)
                      .arg(camera.cols);
        for (int r = 0; r < camera.rows; ++r) {
            QStringList row;
            for (int c = 0; c < camera.cols; ++c) {
                row << formatDouble(camera.at<double>(r, c));
            }
            const bool lastRow = (r + 1 == camera.rows);
            result += QStringLiteral("    %1%2\n")
                          .arg(row.join(QStringLiteral(", ")))
                          .arg(lastRow ? QString() : QStringLiteral(","));
        }
        result += QStringLiteral(");\n\n");
    } else {
        result += QStringLiteral("// cameraMatrix is empty\n\n");
    }

    if (!dist.empty()) {
        const int rows = dist.rows;
        const int cols = dist.cols;
        result += QStringLiteral("cv::Mat distCoeffs = (cv::Mat_<double>(%1, %2) <<\n")
                      .arg(rows)
                      .arg(cols);
        QStringList coeffs;
        const int count = static_cast<int>(dist.total());
        for (int i = 0; i < count; ++i) {
            coeffs << formatDouble(dist.at<double>(i));
        }
        result += QStringLiteral("    %1);\n\n").arg(coeffs.join(QStringLiteral(", ")));
    } else {
        result += QStringLiteral("// distCoeffs is empty\n\n");
    }

    result += QStringLiteral("cv::Size imageSize(%1, %2);\n\n")
                  .arg(m_output.imageSize.width)
                  .arg(m_output.imageSize.height);

    result += QStringLiteral("// Reprojection metrics (pixels)\n");
    result += QStringLiteral("const double rmsPx = %1;\n").arg(formatDouble(m_output.metrics.rms));
    result += QStringLiteral("const double meanPx = %1;\n").arg(formatDouble(m_output.metrics.meanErrorPx));
    result += QStringLiteral("const double medianPx = %1;\n").arg(formatDouble(m_output.metrics.medianErrorPx));
    result += QStringLiteral("const double p95Px = %1;\n").arg(formatDouble(m_output.metrics.p95ErrorPx));
    result += QStringLiteral("const double maxPx = %1;\n").arg(formatDouble(m_output.metrics.maxErrorPx));

    return result;
}

QString ParameterDialog::buildPlainSnippet() const
{
    QString result;
    const cv::Mat camera = toDoubleMat(m_output.cameraMatrix);
    const cv::Mat dist = toDoubleMat(m_output.distCoeffs);

    result += QStringLiteral("Calibration parameters\n");
    result += QStringLiteral("======================\n");

    result += QStringLiteral("Camera matrix:\n");
    if (!camera.empty()) {
        for (int r = 0; r < camera.rows; ++r) {
            QStringList row;
            for (int c = 0; c < camera.cols; ++c) {
                row << formatDouble(camera.at<double>(r, c));
            }
            result += QStringLiteral("  [%1]\n").arg(row.join(QStringLiteral(", ")));
        }
    }
    result += QStringLiteral("\n");

    result += QStringLiteral("Distortion coefficients:\n");
    if (!dist.empty()) {
        QStringList coeffs;
        const int count = static_cast<int>(dist.total());
        for (int i = 0; i < count; ++i) {
            coeffs << formatDouble(dist.at<double>(i));
        }
        result += QStringLiteral("  %1\n\n").arg(coeffs.join(QStringLiteral(", ")));
    } else {
        result += QStringLiteral("  (none)\n\n");
    }

    result += QStringLiteral("Image size : %1 x %2 px\n")
                  .arg(m_output.imageSize.width)
                  .arg(m_output.imageSize.height);
    result += QStringLiteral("RMS error : %1 px\n")
                  .arg(formatDouble(m_output.metrics.rms));
    result += QStringLiteral("Mean error: %1 px\n")
                  .arg(formatDouble(m_output.metrics.meanErrorPx));
    result += QStringLiteral("Median    : %1 px\n")
                  .arg(formatDouble(m_output.metrics.medianErrorPx));
    result += QStringLiteral("P95       : %1 px\n")
                  .arg(formatDouble(m_output.metrics.p95ErrorPx));
    result += QStringLiteral("Max       : %1 px\n")
                  .arg(formatDouble(m_output.metrics.maxErrorPx));

    return result;
}

QString ParameterDialog::formatDouble(double value, int precision) const
{
    QString str = QString::number(value, 'f', precision);
    if (str.contains('.')) {
        while (str.endsWith('0')) {
            str.chop(1);
        }
        if (str.endsWith('.')) {
            str.chop(1);
        }
    }
    if (str.isEmpty()) {
        str = QStringLiteral("0");
    }
    return str;
}

} // namespace mycalib