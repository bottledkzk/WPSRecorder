#ifndef WPSRECORDER_WEBSTUDIOSURFACE_H
#define WPSRECORDER_WEBSTUDIOSURFACE_H

#include <QJsonObject>
#include <QWidget>
#include <memory>

class KRRWebStudioSurface : public QWidget
{
	Q_OBJECT
public:
	explicit KRRWebStudioSurface(QWidget* parent);
	~KRRWebStudioSurface() override;
	void start();
	bool send(const QJsonObject& message);
	QString localResource(const QString& file);
	QString fileForResource(const QString& id) const;
signals:
	void ready();
	void request(const QJsonObject& message);
	void failed(const QString& message);
protected:
	void resizeEvent(QResizeEvent* event) override;
private:
	struct Private;
	std::unique_ptr<Private> p;
};
#endif
