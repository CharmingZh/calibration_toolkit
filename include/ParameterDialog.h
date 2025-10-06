#pragma once

#include <QDialog>

#include "CalibrationEngine.h"

class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QLabel;

namespace mycalib {

class ParameterDialog : public QDialog {
    Q_OBJECT

public:
    explicit ParameterDialog(const CalibrationOutput &output, QWidget *parent = nullptr);

private:
    enum class SnippetStyle {
        Python,
        Cpp,
        PlainText
    };

    void refreshPreview();
    QString buildPythonSnippet() const;
    QString buildCppSnippet() const;
    QString buildPlainSnippet() const;
    QString formatDouble(double value, int precision = 8) const;

    CalibrationOutput m_output;
    QComboBox *m_styleCombo {nullptr};
    QPlainTextEdit *m_preview {nullptr};
    QPushButton *m_copyButton {nullptr};
    QLabel *m_statusLabel {nullptr};
    SnippetStyle m_currentStyle {SnippetStyle::Python};
};

} // namespace mycalib
