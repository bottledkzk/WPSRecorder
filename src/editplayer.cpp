#include "uilanguage.h"
#include "editplayer.h"
#include "recordingconfig.h"
#include <QProcess>
#include <QThread>
#include <cmath>
KRREditPlayer::KRREditPlayer(QObject *parent) : QObject(parent)
{
	connect(&m_timer, &QTimer::timeout, this, &KRREditPlayer::tick);
	m_timer.setInterval(15);
}
KRREditPlayer::~KRREditPlayer()
{
	endDecode();
}
void KRREditPlayer::endDecode()
{
	m_cancel.store(true);
	if (m_thread.joinable())
		m_thread.join();
	std::lock_guard<std::mutex> lock(m_mutex);
	m_frames.clear();
	m_decodeError.clear();
}
void KRREditPlayer::setSource(const QString &path, const QSize &size, double fps, qint64 duration)
{
	stop();
	m_peakFrames.store(0);
	m_source = path;
	m_size = size;
	if (size.width() > 1920 || size.height() > 1080)
		m_size.scale(1920, 1080, Qt::KeepAspectRatio);
	m_size = QSize(qMax(2, m_size.width() / 2 * 2), qMax(2, m_size.height() / 2 * 2));
	m_rate = qMin(60.0, fps);
	m_sourceRate = fps;
	m_duration = duration;
	m_error.clear();
}
void KRREditPlayer::clear()
{
	stop();
	m_source.clear();
}
void KRREditPlayer::setPosition(qint64 time)
{
	m_position = qBound<qint64>(0, time, qMax<qint64>(0, m_duration - 1));
	if (m_state == QMediaPlayer::PlayingState)
		startDecode();
}
void KRREditPlayer::setPlaybackRate(double rate)
{
	if (m_playbackRate == rate) return;
	m_playbackRate = qBound(.5, rate, 2.);
	if (m_clock.isValid())
	{
		m_baseTime = m_position;
		m_clock.restart();
		m_lastTick = 0;
	}
}
void KRREditPlayer::play()
{
	if (m_source.isEmpty() || m_state == QMediaPlayer::PlayingState)
		return;
	m_state = QMediaPlayer::PlayingState;
	startDecode();
	m_timer.start();
	emit stateChanged(m_state);
}
void KRREditPlayer::pause()
{
	m_timer.stop();
	endDecode();
	m_clock.invalidate();
	m_buffering = false;
	if (m_state != QMediaPlayer::PausedState)
	{
		m_state = QMediaPlayer::PausedState;
		emit stateChanged(m_state);
	}
}
void KRREditPlayer::stop()
{
	m_timer.stop();
	endDecode();
	m_clock.invalidate();
	m_buffering = false;
	m_position = 0;
	if (m_state != QMediaPlayer::StoppedState)
	{
		m_state = QMediaPlayer::StoppedState;
		emit stateChanged(m_state);
	}
}
void KRREditPlayer::startDecode()
{
	endDecode();
	m_cancel.store(false);
	m_ended.store(false);
	m_buffering = true;
	m_clock.invalidate();
	m_lastTick = 0;
	QString source = m_source;
	QSize size = m_size;
	double rate = m_rate;
	qint64 start = m_position;
	double seconds = std::floor(qRound64(start * m_sourceRate / 1000.) * 1000000. / m_sourceRate) / 1000000.;
	m_thread = std::thread(
		[this, source, size, rate, start, seconds]
		{
			QProcess process;
			HideConsoleProcess(process);
			process.start(FindFFmpeg(), {"-v", "error", "-nostdin", "-ss", QString::number(seconds, 'f', 6),
										 "-i", source, "-an", "-sn", "-dn", "-vf",
										 QString("setpts=PTS-STARTPTS,scale=%1:%2,fps=%3")
											 .arg(size.width())
											 .arg(size.height())
											 .arg(rate, 0, 'g', 10),
										 "-pix_fmt", "bgra", "-f", "rawvideo", "pipe:1"});
			auto fail = [this](const QString &error)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_decodeError = error;
				m_ended.store(true);
			};
			if (!process.waitForStarted(5000))
			{
				fail(Ui("无法启动剪辑预览解码器。"));
				return;
			}
			QByteArray bytes, errors;
			qint64 index = 0;
			int frameBytes = size.width() * size.height() * 4;
			QElapsedTimer progress;
			progress.start();
			while (!m_cancel.load())
			{
				bool full;
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					full = m_frames.size() >= 4;
				}
				if (full)
				{
					progress.restart();
					QThread::msleep(3);
					continue;
				}
				if (bytes.size() >= frameBytes)
				{
					QImage image = QImage(reinterpret_cast<const uchar *>(bytes.constData()), size.width(),
										  size.height(), QImage::Format_RGB32)
									   .copy();
					bytes.remove(0, frameBytes);
					{
						std::lock_guard<std::mutex> lock(m_mutex);
						m_frames.push_back({start + qRound64(index++ * 1000 / rate), image});
						m_peakFrames.store(qMax(m_peakFrames.load(), static_cast<int>(m_frames.size())));
					}
					continue;
				}
				process.waitForReadyRead(100);
				auto available = process.readAllStandardOutput();
				if (!available.isEmpty())
					progress.restart();
				bytes += available;
				errors += process.readAllStandardError();
				if (errors.size() > 3000)
					errors = errors.right(3000);
				if (process.state() == QProcess::NotRunning && bytes.size() < frameBytes)
				{
					if (process.exitCode() != 0)
						fail(Ui("预览解码失败：") + QString::fromUtf8(errors));
					else
						m_ended.store(true);
					return;
				}
				if (progress.elapsed() > 15000)
				{
					process.kill();
					process.waitForFinished(2000);
					fail(Ui("读取预览画面超时。"));
					return;
				}
			}
			process.kill();
			process.waitForFinished(2000);
		});
}
void KRREditPlayer::tick()
{
	if (m_state != QMediaPlayer::PlayingState)
		return;
	Frame chosen;
	bool found = false, ready = false, empty, finished = false;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		empty = m_frames.empty();
		finished = m_ended.load();
		if (!empty)
		{
			if (!m_clock.isValid())
			{
				m_baseTime = m_frames.front().time;
				m_clock.start();
				m_lastTick = 0;
				m_buffering = false;
				ready = true;
			}
			qint64 target = m_baseTime + qRound64(m_clock.elapsed() * m_playbackRate);
			while (!m_frames.empty() && m_frames.front().time <= target)
			{
				chosen = std::move(m_frames.front());
				m_frames.pop_front();
				found = true;
			}
		}
	}
	if (found)
	{
		m_position = qMin(chosen.time, m_duration - 1);
		m_lastTick = m_clock.elapsed();
		emit frameReady(chosen.image);
		emit positionChanged(m_position);
		if (ready && m_state == QMediaPlayer::PlayingState && !m_buffering)
			emit stateChanged(m_state);
	}
	else if (empty && finished)
	{
		QString error;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			error = m_decodeError;
		}
		if (!error.isEmpty())
		{
			m_error = error;
			pause();
			emit failed(error);
		}
		else
		{
			emit positionChanged(m_duration);
			if (m_state == QMediaPlayer::PlayingState)
				pause();
		}
	}
	else if (empty && m_clock.isValid() && m_clock.elapsed() - m_lastTick > 250)
	{
		m_clock.invalidate();
		m_buffering = true;
		emit stateChanged(m_state);
	}
}
