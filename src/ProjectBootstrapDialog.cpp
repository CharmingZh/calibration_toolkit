#include "ProjectBootstrapDialog.h"
#include "ProjectHistory.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QFont>
#include <QBrush>
#include <QColor>
#include <QLatin1Char>
#include <QStandardPaths>
#include <QStringList>
#include <QSize>
#include <QVBoxLayout>
#include <QtCore/Qt>

namespace {

constexpr const char kSessionFileName[] = "session.json";

QString htmlEscape(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    escaped.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    escaped.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    escaped.replace(QLatin1Char('\''), QStringLiteral("&#39;"));
    return escaped;
}

} // namespace

namespace mycalib {

ProjectBootstrapDialog::ProjectBootstrapDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Project setup"));
    setModal(true);
    resize(560, 420);
    buildUi();
    connectSignals();
    updateState();
    updatePreview();
    populateRecentProjects();
}

ProjectBootstrapResult ProjectBootstrapDialog::run()
{
    ProjectBootstrapResult result;
    if (exec() == QDialog::Accepted) {
        result.accepted = true;
        const bool isNew = m_modeGroup->checkedId() == 0;
        result.createNew = isNew;
        if (isNew) {
            result.projectDirectory = resolvedNewProjectPath();
            result.projectName = m_newNameEdit->text().trimmed();
            result.dataSource = static_cast<ProjectSession::DataSource>(m_dataSourceCombo->currentData().toInt());
        } else {
            result.projectDirectory = QDir::fromNativeSeparators(m_existingDirEdit->text().trimmed());
            result.dataSource = ProjectSession::DataSource::LocalDataset; // will be loaded from session
        }

        result.projectDirectory = QDir::cleanPath(result.projectDirectory);
    }
    return result;
}

void ProjectBootstrapDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);

    m_modeGroup = new QButtonGroup(this);
    m_modeGroup->setExclusive(true);

    auto *newBox = new QGroupBox(tr("Create new project"), this);
    auto *newLayout = new QFormLayout(newBox);
    m_newDirEdit = new QLineEdit(newBox);
    m_newDirEdit->setClearButtonEnabled(true);
    m_newDirEdit->setPlaceholderText(tr("e.g. %1").arg(QDir::toNativeSeparators(defaultProjectsRoot())));
    m_newDirEdit->setText(QDir::toNativeSeparators(defaultProjectsRoot()));
    m_newDirButton = new QPushButton(tr("Browse"), newBox);
    auto *newDirRow = new QWidget(newBox);
    auto *newDirLayout = new QHBoxLayout(newDirRow);
    newDirLayout->setContentsMargins(0, 0, 0, 0);
    newDirLayout->addWidget(m_newDirEdit, 1);
    newDirLayout->addWidget(m_newDirButton);
    newLayout->addRow(tr("Projects root"), newDirRow);

    m_newNameEdit = new QLineEdit(newBox);
    m_newNameEdit->setPlaceholderText(tr("Calibration session name"));
    m_newNameEdit->setClearButtonEnabled(true);
    newLayout->addRow(tr("Project name"), m_newNameEdit);

    m_newPathPreview = new QLabel(newBox);
    m_newPathPreview->setWordWrap(true);
    m_newPathPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_newPathPreview->setObjectName(QStringLiteral("projectPathPreview"));
    m_newPathPreview->setStyleSheet(QStringLiteral("QLabel#projectPathPreview { font-family: 'Consolas', 'Courier New', monospace; color: #cbd4ff; }"));
    newLayout->addRow(tr("Will be created at"), m_newPathPreview);

    m_dataSourceCombo = new QComboBox(newBox);
    m_dataSourceCombo->addItem(tr("Local images (copied into project)"), static_cast<int>(ProjectSession::DataSource::LocalDataset));
    m_dataSourceCombo->addItem(tr("Live capture (connected camera)"), static_cast<int>(ProjectSession::DataSource::ConnectedCamera));
    newLayout->addRow(tr("Capture source"), m_dataSourceCombo);

    auto *newRadio = new QRadioButton(tr("Start with a new project"), newBox);
    newRadio->setChecked(true);
    m_modeGroup->addButton(newRadio, 0);
    auto *newHeader = new QHBoxLayout();
    newHeader->addWidget(newRadio);
    newHeader->addStretch(1);

    auto *newContainer = new QVBoxLayout();
    newContainer->setContentsMargins(16, 0, 0, 0);
    newContainer->addLayout(newLayout);
    auto *newOuter = new QVBoxLayout(newBox);
    newOuter->addLayout(newHeader);
    newOuter->addLayout(newContainer);
    newOuter->setContentsMargins(12, 12, 12, 12);

    auto *existingBox = new QGroupBox(tr("Open existing project"), this);
    auto *existingLayout = new QFormLayout(existingBox);
    m_existingDirEdit = new QLineEdit(existingBox);
    m_existingDirEdit->setPlaceholderText(tr("Folder with session.json"));
    m_existingDirEdit->setClearButtonEnabled(true);
    m_existingDirButton = new QPushButton(tr("Browse"), existingBox);
    auto *existingRow = new QWidget(existingBox);
    auto *existingRowLayout = new QHBoxLayout(existingRow);
    existingRowLayout->setContentsMargins(0, 0, 0, 0);
    existingRowLayout->addWidget(m_existingDirEdit, 1);
    existingRowLayout->addWidget(m_existingDirButton);
    existingLayout->addRow(tr("Project folder"), existingRow);

    auto *existingRadio = new QRadioButton(tr("Continue with an existing project"), existingBox);
    m_modeGroup->addButton(existingRadio, 1);
    auto *existingHeader = new QHBoxLayout();
    existingHeader->addWidget(existingRadio);
    existingHeader->addStretch(1);

    auto *existingContainer = new QVBoxLayout();
    existingContainer->setContentsMargins(16, 0, 0, 0);
    existingContainer->addLayout(existingLayout);
    auto *existingOuter = new QVBoxLayout(existingBox);
    existingOuter->addLayout(existingHeader);
    existingOuter->addLayout(existingContainer);
    existingOuter->setContentsMargins(12, 12, 12, 12);

    layout->addWidget(newBox);
    layout->addWidget(existingBox);

    auto *recentHeader = new QLabel(tr("Recent projects"), this);
    QFont headerFont = recentHeader->font();
    headerFont.setBold(true);
    recentHeader->setFont(headerFont);
    layout->addWidget(recentHeader);

    m_recentList = new QListWidget(this);
    m_recentList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recentList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recentList->setAlternatingRowColors(true);
    m_recentList->setMinimumHeight(140);
    m_recentList->setUniformItemSizes(true);
    m_recentList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_recentList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    layout->addWidget(m_recentList);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        QString error;
        if (!validateInputs(&error)) {
            QMessageBox::warning(this, tr("Invalid choice"), error);
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &ProjectBootstrapDialog::reject);
}

void ProjectBootstrapDialog::connectSignals()
{
    connect(m_modeGroup,
            static_cast<void (QButtonGroup::*)(QAbstractButton *)>(&QButtonGroup::buttonClicked),
            this,
            [this](QAbstractButton *) {
                updateState();
            });
    connect(m_newDirEdit, &QLineEdit::textChanged, this, [this]() {
        updatePreview();
    });
    connect(m_newNameEdit, &QLineEdit::textChanged, this, [this]() {
        updatePreview();
    });
    connect(m_newDirButton, &QPushButton::clicked, this, &ProjectBootstrapDialog::chooseDirectory);
    connect(m_existingDirButton, &QPushButton::clicked, this, [this]() {
        if (m_modeGroup->checkedId() != 1) {
            if (auto *existing = m_modeGroup->button(1)) {
                existing->setChecked(true);
            }
            updateState();
        }
        chooseExistingDirectory();
    });
    if (m_recentList) {
        connect(m_recentList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
            if (!item || !(item->flags() & Qt::ItemIsSelectable)) {
                return;
            }
            selectExistingProject(item->data(Qt::UserRole).toString());
        });
        connect(m_recentList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
            if (!item || !(item->flags() & Qt::ItemIsSelectable)) {
                return;
            }
            selectExistingProject(item->data(Qt::UserRole).toString());
            QString error;
            if (validateInputs(&error)) {
                accept();
            } else if (!error.isEmpty()) {
                QMessageBox::warning(this, tr("Invalid choice"), error);
            }
        });
    }
}

void ProjectBootstrapDialog::updateState()
{
    const bool isNew = m_modeGroup->checkedId() == 0;
    m_newDirEdit->setEnabled(isNew);
    m_newDirButton->setEnabled(isNew);
    m_newNameEdit->setEnabled(isNew);
    m_dataSourceCombo->setEnabled(isNew);
    if (m_newPathPreview) {
        m_newPathPreview->setEnabled(isNew);
    }
    m_existingDirEdit->setEnabled(true);
    m_existingDirEdit->setReadOnly(isNew);
    m_existingDirButton->setEnabled(true);
    updatePreview();
}

void ProjectBootstrapDialog::updatePreview()
{
    if (!m_newPathPreview) {
        return;
    }

    const bool isNew = !m_modeGroup || m_modeGroup->checkedId() == 0;
    if (!isNew) {
        m_newPathPreview->setTextFormat(Qt::PlainText);
        m_newPathPreview->setText(tr("Switch to \"Start with a new project\" to configure a workspace."));
        return;
    }

    const QString path = resolvedNewProjectPath();
    if (path.isEmpty()) {
        m_newPathPreview->setTextFormat(Qt::PlainText);
        m_newPathPreview->setText(tr("Select a projects root and enter a name."));
        return;
    }

    m_newPathPreview->setTextFormat(Qt::RichText);
    QString rawName = m_newNameEdit ? m_newNameEdit->text().trimmed() : QString();
    QString sanitizedName;
    if (!rawName.isEmpty()) {
        sanitizedName = sanitizedFolderName(m_newNameEdit->text());
    }
    QString message = tr("<code>%1</code>")
                        .arg(htmlEscape(QDir::toNativeSeparators(path)));

    const QFileInfo info(path);
    if (info.exists()) {
        QDir dir(path);
        if (dir.exists(QString::fromUtf8(kSessionFileName))) {
            message += tr("<br/><span style=\"color:#d17b0f\">Project already exists here.</span>");
        } else {
            const QStringList entries = dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
            if (!entries.isEmpty()) {
                message += tr("<br/><span style=\"color:#d17b0f\">Warning: folder is not empty.</span>");
            } else {
                message += tr("<br/><span style=\"color:#7f8c8d\">Existing empty folder will be reused.</span>");
            }
        }
    } else {
        message += tr("<br/><span style=\"color:#7f8c8d\">Folder will be created automatically.</span>");
    }

    if (!rawName.isEmpty() && !sanitizedName.isEmpty() && rawName != sanitizedName) {
    message += tr("<br/><span style=\"color:#9fb6fa\">Folder name adjusted to <code>%1</code>.</span>")
               .arg(htmlEscape(sanitizedName));
    }

    m_newPathPreview->setText(message);
}

QString ProjectBootstrapDialog::defaultProjectsRoot() const
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    QDir dir(base);
    return QDir::cleanPath(dir.filePath(QStringLiteral("MyCalib Projects")));
}

QString ProjectBootstrapDialog::resolvedNewProjectPath() const
{
    if (!m_newDirEdit || !m_newNameEdit) {
        return {};
    }

    const QString base = QDir::fromNativeSeparators(m_newDirEdit->text().trimmed());
    if (base.isEmpty()) {
        return {};
    }

    const QString folder = sanitizedFolderName(m_newNameEdit->text());
    if (folder.isEmpty()) {
        return {};
    }

    QDir dir(base);
    return QDir::cleanPath(dir.filePath(folder));
}

QString ProjectBootstrapDialog::sanitizedFolderName(const QString &name) const
{
    QString trimmed = name;
    trimmed = trimmed.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    QString candidate = trimmed;
    candidate.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    candidate = candidate.simplified();
    candidate.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral(" "));
    candidate.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9 _-]+")), QString());
    candidate = candidate.simplified();
    candidate.replace(QChar(' '), QChar('_'));
    candidate.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    candidate.replace(QRegularExpression(QStringLiteral("-+")), QStringLiteral("-"));

    while (!candidate.isEmpty() && (candidate.front() == '_' || candidate.front() == '-')) {
        candidate.remove(0, 1);
    }
    while (!candidate.isEmpty() && (candidate.back() == '_' || candidate.back() == '-')) {
        candidate.chop(1);
    }

    if (candidate.isEmpty()) {
        candidate = QStringLiteral("MyCalibProject");
    }

    static constexpr int kMaxLength = 60;
    if (candidate.size() > kMaxLength) {
        candidate.truncate(kMaxLength);
    }

    return candidate;
}

void ProjectBootstrapDialog::populateRecentProjects()
{
    if (!m_recentList) {
        return;
    }

    m_recentList->clear();
    const QVector<ProjectHistoryEntry> entries = loadProjectHistory();
    if (entries.isEmpty()) {
        auto *placeholder = new QListWidgetItem(tr("No recent projects yet."), m_recentList);
        placeholder->setFlags(Qt::NoItemFlags);
        placeholder->setTextAlignment(Qt::AlignCenter);
        return;
    }

    for (const ProjectHistoryEntry &entry : entries) {
        const QString displayName = entry.name.isEmpty() ? tr("(Untitled project)") : entry.name;
        const QString pathDisplay = QDir::toNativeSeparators(entry.path);
        const QString subtitle = entry.lastOpened.isValid()
            ? tr("Last opened %1").arg(entry.lastOpened.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
            : tr("Last opened: unknown");

        auto *item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(displayName, pathDisplay), m_recentList);
        item->setData(Qt::UserRole, entry.path);
        const bool available = QDir(entry.path).exists(QString::fromUtf8(kSessionFileName));
        item->setToolTip(QStringLiteral("%1\n%2").arg(subtitle, pathDisplay));
        if (!available) {
            item->setForeground(QBrush(QColor(QStringLiteral("#f39c12"))));
            item->setToolTip(tr("Project files are missing at:") + QStringLiteral("\n%1").arg(pathDisplay));
        }
        item->setSizeHint(QSize(item->sizeHint().width(), 48));
    }
}

void ProjectBootstrapDialog::selectExistingProject(const QString &path)
{
    const QString normalized = QDir::cleanPath(path);
    if (normalized.isEmpty()) {
        return;
    }

    if (auto *existing = m_modeGroup ? m_modeGroup->button(1) : nullptr) {
        existing->setChecked(true);
    }

    if (m_existingDirEdit) {
        m_existingDirEdit->setText(QDir::toNativeSeparators(normalized));
        m_existingDirEdit->setFocus(Qt::OtherFocusReason);
        m_existingDirEdit->selectAll();
    }

    updateState();
}

void ProjectBootstrapDialog::chooseDirectory()
{
    QString start = QDir::fromNativeSeparators(m_newDirEdit->text().trimmed());
    if (start.isEmpty()) {
        start = defaultProjectsRoot();
    }
    const QString dir = QFileDialog::getExistingDirectory(this,
                                                          tr("Select projects root"),
                                                          QDir::toNativeSeparators(start));
    if (!dir.isEmpty()) {
        m_newDirEdit->setText(QDir::toNativeSeparators(dir));
    }
}

void ProjectBootstrapDialog::chooseExistingDirectory()
{
    const QString start = QDir::fromNativeSeparators(m_existingDirEdit->text().trimmed());
    const QString dir = QFileDialog::getExistingDirectory(this,
                                                          tr("Open project folder"),
                                                          start.isEmpty() ? QString() : QDir::toNativeSeparators(start));
    if (!dir.isEmpty()) {
        m_existingDirEdit->setText(QDir::toNativeSeparators(dir));
    }
}

bool ProjectBootstrapDialog::validateInputs(QString *error) const
{
    const bool isNew = m_modeGroup->checkedId() == 0;
    if (isNew) {
        const QString base = QDir::fromNativeSeparators(m_newDirEdit->text().trimmed());
        if (base.isEmpty()) {
            if (error) {
                *error = tr("Please choose a projects root folder.");
            }
            return false;
        }

        const QString name = m_newNameEdit->text().trimmed();
        if (name.isEmpty()) {
            if (error) {
                *error = tr("Please enter a project name.");
            }
            return false;
        }

        const QString folder = resolvedNewProjectPath();
        if (folder.isEmpty()) {
            if (error) {
                *error = tr("Could not resolve the project folder. Please adjust the name.");
            }
            return false;
        }

        const QFileInfo finalInfo(folder);
        if (finalInfo.exists()) {
            if (!finalInfo.isDir()) {
                if (error) {
                    *error = tr("Target path %1 is not a directory.").arg(QDir::toNativeSeparators(folder));
                }
                return false;
            }

            QDir finalDir(folder);
            if (finalDir.exists(QString::fromUtf8(kSessionFileName))) {
                if (error) {
                    *error = tr("A MyCalib project already exists in %1. Choose a different name or open it.")
                                 .arg(QDir::toNativeSeparators(folder));
                }
                return false;
            }

            const QStringList entries = finalDir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
            if (!entries.isEmpty()) {
                if (error) {
                    *error = tr("The folder %1 is not empty. Please select an empty location.")
                                 .arg(QDir::toNativeSeparators(folder));
                }
                return false;
            }
        } else {
            QDir parent = finalInfo.dir();
            if (!parent.exists()) {
                if (!QDir().mkpath(parent.absolutePath())) {
                    if (error) {
                        *error = tr("Cannot create parent directory %1.")
                                     .arg(QDir::toNativeSeparators(parent.absolutePath()));
                    }
                    return false;
                }
            }
        }

        return true;
    }

    const QString directory = QDir::fromNativeSeparators(m_existingDirEdit->text().trimmed());
    if (directory.isEmpty()) {
        if (error) {
            *error = tr("Please choose an existing project folder.");
        }
        return false;
    }
    QDir dir(directory);
    if (!dir.exists(QString::fromUtf8(kSessionFileName))) {
        if (error) {
            *error = tr("The selected folder does not contain a MyCalib project (missing session.json).");
        }
        return false;
    }
    return true;
}

} // namespace mycalib
