#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFontDatabase>
#include <QMessageBox>
#include <QTextStream>

#include <cmath>

#include "CalibrationEngine.h"
#include "MainWindow.h"
#include "ProjectBootstrapDialog.h"
#include "ProjectHistory.h"
#include "ProjectSession.h"

namespace {

bool wantsBatchMode(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--batch") || arg == QStringLiteral("-b") ||
            arg.startsWith(QStringLiteral("--input")) || arg == QStringLiteral("-i") ||
            arg.startsWith(QStringLiteral("--output")) || arg == QStringLiteral("-o")) {
            return true;
        }
    }
    return false;
}

int runBatchMode(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("MyCalib Batch"));
    QCoreApplication::setOrganizationName(QStringLiteral("CalibLab"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("MyCalib headless calibration pipeline"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption batchOption({QStringLiteral("b"), QStringLiteral("batch")},
                                   QStringLiteral("Run calibration without launching the GUI."));
    QCommandLineOption inputOption({QStringLiteral("i"), QStringLiteral("input")},
                                   QStringLiteral("Directory containing calibration images."),
                                   QStringLiteral("dir"));
    QCommandLineOption outputOption({QStringLiteral("o"), QStringLiteral("output")},
                                    QStringLiteral("Directory where reports and figures will be written."),
                                    QStringLiteral("dir"));
    QCommandLineOption diameterOption({QStringLiteral("d"), QStringLiteral("diameter")},
                                      QStringLiteral("Small circle diameter in millimetres."),
                                      QStringLiteral("mm"));
    QCommandLineOption spacingOption({QStringLiteral("s"), QStringLiteral("spacing")},
                                     QStringLiteral("Circle centre spacing in millimetres."),
                                     QStringLiteral("mm"));
    QCommandLineOption maxMeanOption({QStringLiteral("M"), QStringLiteral("max-mean")},
                                     QStringLiteral("Maximum mean reprojection error threshold (pixels)."),
                                     QStringLiteral("px"));
    QCommandLineOption maxPointOption({QStringLiteral("P"), QStringLiteral("max-point")},
                                      QStringLiteral("Maximum per-point reprojection error threshold (pixels)."),
                                      QStringLiteral("px"));
    QCommandLineOption minSamplesOption({QStringLiteral("m"), QStringLiteral("min-samples")},
                                        QStringLiteral("Minimum number of successful detections required."),
                                        QStringLiteral("count"));
    QCommandLineOption maxIterationsOption({QStringLiteral("I"), QStringLiteral("max-iterations")},
                                           QStringLiteral("Maximum number of outlier removal iterations."),
                                           QStringLiteral("count"));
    QCommandLineOption noRefineOption(QStringLiteral("no-refine"),
                                      QStringLiteral("Disable the non-linear refinement stage."));

    parser.addOption(batchOption);
    parser.addOption(inputOption);
    parser.addOption(outputOption);
    parser.addOption(diameterOption);
    parser.addOption(spacingOption);
    parser.addOption(maxMeanOption);
    parser.addOption(maxPointOption);
    parser.addOption(minSamplesOption);
    parser.addOption(maxIterationsOption);
    parser.addOption(noRefineOption);

    parser.process(app);

    if (!parser.isSet(inputOption) || !parser.isSet(outputOption)) {
        QTextStream(stderr) << "Error: --input and --output must be provided in batch mode." << Qt::endl;
        parser.showHelp(1);
    }

    mycalib::CalibrationEngine::Settings settings;

    auto parsePositiveDouble = [&](const QCommandLineOption &option, double &target) -> bool {
        if (!parser.isSet(option)) {
            return true;
        }
        bool ok = false;
        const double value = parser.value(option).toDouble(&ok);
        if (!ok || !std::isfinite(value) || value <= 0.0) {
            const QStringList names = option.names();
            const QString flag = names.isEmpty() ? QStringLiteral("?") : QStringLiteral("--") + names.first();
            QTextStream(stderr) << "Invalid value for " << flag << ": " << parser.value(option) << Qt::endl;
            return false;
        }
        target = value;
        return true;
    };

    auto parsePositiveInt = [&](const QCommandLineOption &option, int &target) -> bool {
        if (!parser.isSet(option)) {
            return true;
        }
        bool ok = false;
        const int value = parser.value(option).toInt(&ok);
        if (!ok || value <= 0) {
            const QStringList names = option.names();
            const QString flag = names.isEmpty() ? QStringLiteral("?") : QStringLiteral("--") + names.first();
            QTextStream(stderr) << "Invalid value for " << flag << ": " << parser.value(option) << Qt::endl;
            return false;
        }
        target = value;
        return true;
    };

    if (!parsePositiveDouble(diameterOption, settings.boardSpec.smallDiameterMm) ||
        !parsePositiveDouble(spacingOption, settings.boardSpec.centerSpacingMm) ||
        !parsePositiveDouble(maxMeanOption, settings.maxMeanErrorPx) ||
        !parsePositiveDouble(maxPointOption, settings.maxPointErrorPx) ||
        !parsePositiveInt(minSamplesOption, settings.minSamples) ||
        !parsePositiveInt(maxIterationsOption, settings.maxIterations)) {
        return 1;
    }

    if (parser.isSet(noRefineOption)) {
        settings.enableRefinement = false;
    }

    const QString inputDir = parser.value(inputOption);
    const QString outputDir = parser.value(outputOption);

    mycalib::CalibrationEngine engine;
    const auto result = engine.runBlocking(inputDir, settings, outputDir);
    if (!result.success) {
        QTextStream(stderr) << "Calibration failed: " << result.message << Qt::endl;
        return 2;
    }

    QTextStream(stdout) << "Calibration succeeded. Results written to "
                        << outputDir << Qt::endl;
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    if (wantsBatchMode(argc, argv)) {
        return runBatchMode(argc, argv);
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("MyCalib GUI"));
    QApplication::setOrganizationName(QStringLiteral("CalibLab"));
    QApplication::setStyle(QStringLiteral("Fusion"));

    QFile themeFile(QStringLiteral(":/theme/palettes.qss"));
    themeFile.open(QIODevice::ReadOnly | QIODevice::Text);
    if (themeFile.isOpen()) {
        app.setStyleSheet(QString::fromUtf8(themeFile.readAll()));
    }

    mycalib::ProjectSession session;

    while (true) {
        mycalib::ProjectBootstrapDialog dialog;
        const auto choice = dialog.run();
        if (!choice.accepted) {
            return 0;
        }

        QString error;
        bool ok = false;
        if (choice.createNew) {
            ok = session.initializeNew(choice.projectDirectory,
                                       choice.projectName,
                                       choice.dataSource,
                                       &error);
        } else {
            ok = session.loadExisting(choice.projectDirectory, &error);
        }

        if (ok) {
            mycalib::recordProjectHistoryEntry(session.rootPath(), session.metadata().projectName);
            break;
        }

        QMessageBox::critical(nullptr,
                               QObject::tr("Project error"),
                               error.isEmpty() ? QObject::tr("Failed to load project.") : error);
    }

    mycalib::MainWindow window(&session);
    window.resize(1480, 940);
    window.show();

    return app.exec();
}
