#ifndef WPSRECORDER_CAMERACAPTURE_H
#define WPSRECORDER_CAMERACAPTURE_H
#include <QImage>
#include <QJsonArray>
#include <QString>
#include <memory>
// Media Foundation owns the device in a dedicated COM thread; no frames leave the PC.
class KRRCameraCapture
{
public:
	KRRCameraCapture();
	~KRRCameraCapture();
	bool start(int index, QString& error);
	void startAsync(int index);
	void stopAsync();
	bool isRunning() const;
	void stop();
	QImage frame(QString& error, quint64* sequence = nullptr);
	quint64 frameSequence() const;
	static QJsonArray Devices();
private:
	struct State;
	std::unique_ptr<State> m_state;
};
#endif
