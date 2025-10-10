#pragma once
#include <QWidget>
#include <QMap>
#include <QPointer>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:5054)
#endif
#include <VmbCPP/VmbCPP.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

class QLabel;
class QTabWidget;


class FeaturePanel : public QWidget {
	Q_OBJECT
public:
	explicit FeaturePanel(VmbCPP::CameraPtr cam, QWidget* parent=nullptr);

	void setCamera(VmbCPP::CameraPtr cam);

Q_SIGNALS:
	void logMsg(const QString& m);

public Q_SLOTS:
	void refresh();

private:
	QWidget* makeEditor(const VmbCPP::FeaturePtr& f);
	void configureEditorWidget(QWidget* widget) const;

	VmbCPP::CameraPtr m_cam;
	QTabWidget* m_tabWidget{nullptr};
	QLabel* m_messageLabel{nullptr};
};
