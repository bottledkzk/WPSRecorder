#ifndef WPSRECORDER_PREVIEWVIDEO_H
#define WPSRECORDER_PREVIEWVIDEO_H

#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QWidget>
#include <memory>

class QAbstractVideoSurface;
class QMediaPlayer;
class QTemporaryDir;

bool PrefersMediaFoundationPlayback();
bool NeedsCompatiblePreview(const QString& file);

class KRRCompatiblePreview : public QObject
{
	Q_OBJECT
public:
	KRRCompatiblePreview();
	void prepare(const QString& source, bool audioOnly = false);
	void clear();
	bool isPreparing() const { return !m_process.isNull(); }
	QString file() const { return m_file; }
	bool retryWithEncoding();

signals:
	void ready(const QString& file);
	void failed(const QString& error);

private:
	void startProcess(bool encode);
	void removeRetiredFiles();
	QPointer<QProcess> m_process;
	std::shared_ptr<QTemporaryDir> m_workspace;
	QVector<std::shared_ptr<QTemporaryDir>> m_retired;
	QTimer m_cleanupTimer;
	QString m_source, m_file;
	bool m_audioOnly = false, m_encoded = false;
};

// Preview only. Recording and live monitoring do not use this surface.
class KRRPreviewVideo : public QWidget
{
public:
	explicit KRRPreviewVideo(QWidget* parent = nullptr);
	void attach(QMediaPlayer* player);
	void clear();
	quint64 frameCount() const { return m_frameCount; }
	int recoveryCount() const { return m_recoveryCount; }

protected:
	void paintEvent(QPaintEvent*) override;

private:
	class Surface;
	struct Frame
	{
		QImage image;
		QRect viewport;
		bool bottomUp = false;
	};
	void deliver(Frame frame);
	void checkPlayback();
	Surface* m_surface;
	Frame m_frame, m_pending;
	QMutex m_mutex;
	bool m_scheduled = false;
	quint64 m_frameCount = 0;
	QPointer<QMediaPlayer> m_player;
	QTimer m_watchdog;
	QElapsedTimer m_lastFrame;
	quint64 m_observedFrame = 0;
	int m_recoveryCount = 0;
	bool m_attemptedRecovery = false, m_recovering = false;
};

void ChangePreviewRate(QMediaPlayer* player, qreal rate);

#endif
