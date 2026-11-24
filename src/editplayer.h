#ifndef WPSRECORDER_EDITPLAYER_H
#define WPSRECORDER_EDITPLAYER_H
#include <QElapsedTimer>
#include <QImage>
#include <QMediaPlayer>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
// Bounded FFmpeg decode: the displayed frame owns the preview clock.
class KRREditPlayer : public QObject
{
	Q_OBJECT
  public:
	explicit KRREditPlayer(QObject *parent = nullptr);
	~KRREditPlayer() override;
	void setSource(const QString &path, const QSize &size, double fps, qint64 duration);
	void clear();
	void setPosition(qint64 time);
	void setPlaybackRate(double rate);
	double playbackRate() const { return m_playbackRate; }
	void play();
	void pause();
	void stop();
	QMediaPlayer::State state() const
	{
		return m_state;
	}
	bool isBuffering() const
	{
		return m_buffering;
	}
	QString errorString() const
	{
		return m_error;
	}
	qint64 position() const
	{
		return m_position;
	}
	int peakBufferedFrames() const
	{
		return m_peakFrames.load();
	}
  signals:
	void frameReady(const QImage &image);
	void positionChanged(qint64 time);
	void stateChanged(QMediaPlayer::State state);
	void failed(const QString &error);

  private:
	struct Frame
	{
		qint64 time;
		QImage image;
	};
	void startDecode();
	void endDecode();
	void tick();
	QString m_source, m_error, m_decodeError;
	QSize m_size;
	double m_rate = 30;
	double m_sourceRate = 30;
	double m_playbackRate = 1.;
	qint64 m_duration = 0, m_position = 0, m_baseTime = 0, m_lastTick = 0;
	QMediaPlayer::State m_state = QMediaPlayer::StoppedState;
	bool m_buffering = false;
	QTimer m_timer;
	QElapsedTimer m_clock;
	std::thread m_thread;
	std::atomic<bool> m_cancel{false}, m_ended{false};
	std::atomic<int> m_peakFrames{0};
	std::mutex m_mutex;
	std::deque<Frame> m_frames;
};
#endif
