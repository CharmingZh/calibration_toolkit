#pragma once

#include <QDialog>
#include <QString>

#include "ProjectSession.h"

class QButtonGroup;
class QComboBox;
class QLineEdit;
class QLabel;
class QListWidget;
class QPushButton;

namespace mycalib {

struct ProjectBootstrapResult {
    bool accepted {false};
    bool createNew {false};
    QString projectDirectory;
    QString projectName;
    ProjectSession::DataSource dataSource {ProjectSession::DataSource::LocalDataset};
};

class ProjectBootstrapDialog : public QDialog {
    Q_OBJECT

public:
    explicit ProjectBootstrapDialog(QWidget *parent = nullptr);

    ProjectBootstrapResult run();

private:
    void buildUi();
    void connectSignals();
    void updateState();
    void updatePreview();
    void chooseDirectory();
    void chooseExistingDirectory();
    bool validateInputs(QString *error) const;
    QString defaultProjectsRoot() const;
    QString resolvedNewProjectPath() const;
    QString sanitizedFolderName(const QString &name) const;
    void populateRecentProjects();
    void selectExistingProject(const QString &path);

    QButtonGroup *m_modeGroup {nullptr};
    QLineEdit *m_newDirEdit {nullptr};
    QLineEdit *m_newNameEdit {nullptr};
    QLineEdit *m_existingDirEdit {nullptr};
    QComboBox *m_dataSourceCombo {nullptr};
    QPushButton *m_newDirButton {nullptr};
    QPushButton *m_existingDirButton {nullptr};
    QLabel *m_newPathPreview {nullptr};
    QListWidget *m_recentList {nullptr};
};

} // namespace mycalib
