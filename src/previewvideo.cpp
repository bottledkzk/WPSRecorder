#include "previewvideo.h"
#include "recordingconfig.h"
#include "uilanguage.h"

#include <QAbstractVideoSurface>
#include <QDir>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QPainter>
#include <QScopedValueRollback>
#include <QTemporaryDir>
#include <QThread>
#include <QVideoFrame>
#include <QVideoSurfaceFormat>
#include <algorithm>

bool PrefersMediaFoundationPlayback()
{
	return qEnvironmentVariable("QT_MULTIMEDIA_PREFERRED_PLUGINS").startsWith("windowsmediafoundation");
}

bool NeedsCompatiblePreview(const QString& file)
{
	const auto format = QFileInfo(file).suffix().toLower();
	return PrefersMediaFoundationPlayback() && (format == "avi" || format == "flv");
}

KRRCompatiblePreview::KRRCompatiblePreview()
{
	m_cleanupTimer.setInterval(350);
	connect(&m_cleanupTimer, &QTimer::timeout, this, &KRRCompatiblePreview::removeRetiredFiles);
}

void KRRCompatiblePreview::prepare(const QString& source, bool audioOnly)
{
	clear();
	m_workspace = std::make_shared<QTemporaryDir>();
	if (!m_workspace->isValid())
	{
		emit failed(Ui("无法创建兼容预览目录。"));
		return;
	}
	m_source = source;
	m_audioOnly = audioOnly;
	startProcess(false);
}

void KRRCompatiblePreview::clear()
{
	auto process = m_process;
	m_process.clear();
	if (process && process->state() != QProcess::NotRunning) process->kill();
	if (m_workspace && m_workspace->isValid())
		m_retired.append(std::move(m_workspace));
	else m_workspace.reset();
	m_file.clear();
	removeRetiredFiles();
}

void KRRCompatiblePreview::removeRetiredFiles()
{
	// Media Foundation may release its file handle after setMedia() returns.
	const auto released = std::remove_if(m_retired.begin(), m_retired.end(), [](const auto& folder)
	{
		return QDir(folder->path()).removeRecursively();
	});
	m_retired.erase(released, m_retired.end());
	if (m_retired.isEmpty()) m_cleanupTimer.stop();
	else if (!m_cleanupTimer.isActive()) m_cleanupTimer.start();
}

bool KRRCompatiblePreview::retryWithEncoding()
{
	if (!m_workspace || m_file.isEmpty() || m_encoded || isPreparing()) return false;
	m_file.clear();
	startProcess(true);
	return true;
}

void KRRCompatiblePreview::startProcess(bool encode)
{
	m_encoded = encode;
	const auto workspace = m_workspace;
	const auto output = workspace->filePath(m_audioOnly ? (encode ? "audio-encoded.m4a" : "audio.m4a") :
		(encode ? "preview-encoded.mp4" : "preview.mp4"));
	auto process = new QProcess(this);
	m_process = process;
	HideConsoleProcess(*process, true);
	connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
		[this, process, workspace, output, encode](int code, QProcess::ExitStatus status)
	{
		const auto error = QString::fromLocal8Bit(process->readAllStandardError().right(2000)).trimmed();
		process->deleteLater();
		if (m_process != process || m_workspace != workspace) return;
		m_process.clear();
		if (code == 0 && status == QProcess::NormalExit && QFileInfo(output).size() > 0)
		{
			m_file = output;
			emit ready(output);
		}
		else if (!encode) startProcess(true);
		else emit failed(Ui("无法生成兼容预览。") + (error.isEmpty() ? QString() : "\n" + error));
	});
	connect(process, &QProcess::errorOccurred, this, [this, process, workspace](QProcess::ProcessError error)
	{
		if (error != QProcess::FailedToStart) return;
		process->deleteLater();
		if (m_process != process || m_workspace != workspace) return;
		m_process.clear();
		emit failed(Ui("无法启动兼容预览转换。"));
	});

	QStringList args{"-v", "error", "-nostdin", "-n", "-threads", "2", "-i", m_source};
	if (m_audioOnly)
		args << "-map" << "0:a:0" << "-vn" << "-c:a" << (encode ? "aac" : "copy");
	else
	{
		args << "-map" << "0:v:0" << "-map" << "0:a:0?";
		if (!encode) args << "-c" << "copy";
		else args << "-filter_threads" << "2" << "-vf"
				  << "scale=w='min(1920,iw)':h='min(1080,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2:flags=lanczos"
				  << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "18" << "-threads" << "2"
				  << "-pix_fmt" << "yuv420p" << "-c:a" << "aac";
	}
	args << output;
	process->start(FindFFmpeg(), args);
}

class KRRPreviewVideo::Surface : public QAbstractVideoSurface
{
public:
	explicit Surface(KRRPreviewVideo* owner) : QAbstractVideoSurface(owner), m_owner(owner) {}
	QList<QVideoFrame::PixelFormat> supportedPixelFormats(QAbstractVideoBuffer::HandleType handle) const override
	{
		if (handle != QAbstractVideoBuffer::NoHandle) return {};
		return {QVideoFrame::Format_RGB32, QVideoFrame::Format_ARGB32, QVideoFrame::Format_RGB24,
			QVideoFrame::Format_RGB565, QVideoFrame::Format_RGB555};
	}
	bool present(const QVideoFrame& frame) override
	{
		if (!frame.isValid()) return true; // A transition should leave the last good frame visible.
		const auto imageFormat = QVideoFrame::imageFormatFromPixelFormat(frame.pixelFormat());
		if (imageFormat == QImage::Format_Invalid) return false;
		auto mapped = frame;
		if (!mapped.map(QAbstractVideoBuffer::ReadOnly)) return false;
		QImage image;
		if (mapped.bits() && mapped.bytesPerLine() > 0 && mapped.height() > 0 &&
			qint64(mapped.bytesPerLine()) * mapped.height() <= mapped.mappedBytes())
		{
			const QImage view(mapped.bits(), mapped.width(), mapped.height(), mapped.bytesPerLine(), imageFormat);
			// Keep owned pixels so playback never holds a DirectShow decoder sample between callbacks.
			image = view.copy();
		}
		mapped.unmap();
		if (image.isNull()) return false;
		const auto format = surfaceFormat();
		m_owner->deliver({std::move(image), format.viewport(), format.scanLineDirection() == QVideoSurfaceFormat::BottomToTop});
		return true;
	}

private:
	KRRPreviewVideo* m_owner;
};

KRRPreviewVideo::KRRPreviewVideo(QWidget* parent) : QWidget(parent), m_surface(new Surface(this))
{
	setAttribute(Qt::WA_OpaquePaintEvent);
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
	m_watchdog.setInterval(180);
	connect(&m_watchdog, &QTimer::timeout, this, &KRRPreviewVideo::checkPlayback);
}

void KRRPreviewVideo::attach(QMediaPlayer* player)
{
	m_player = player;
	player->setVideoOutput(m_surface);
	connect(player, &QMediaPlayer::stateChanged, this, [this](QMediaPlayer::State state)
	{
		if (m_recovering) return;
		if (state == QMediaPlayer::PlayingState)
		{
			m_observedFrame = m_frameCount;
			m_lastFrame.restart();
			m_attemptedRecovery = false;
			m_watchdog.start();
		}
		else m_watchdog.stop();
	});
}

void KRRPreviewVideo::checkPlayback()
{
	if (m_recovering || !m_player || m_player->state() != QMediaPlayer::PlayingState) return;
	if (!isVisible() || m_observedFrame != m_frameCount)
	{
		m_observedFrame = m_frameCount;
		m_lastFrame.restart();
		m_attemptedRecovery = false;
		return;
	}
	if (m_attemptedRecovery || m_lastFrame.elapsed() < 650 || !m_player->isSeekable() ||
		!m_player->isVideoAvailable() || m_player->duration() - m_player->position() < 100) return;
	auto player = m_player.data();
	const auto at = player->position();
	const QScopedValueRollback<bool> recovering(m_recovering, true);
	m_attemptedRecovery = true;
	++m_recoveryCount;
	// A DirectShow stop or surface change can wait indefinitely on stalled filters.
	player->setPosition(qMin(at + 1, player->duration() - 1));
}

void KRRPreviewVideo::clear()
{
	QMutexLocker lock(&m_mutex);
	m_pending = {};
	m_frame = {};
	update();
}

void KRRPreviewVideo::deliver(Frame frame)
{
	if (QThread::currentThread() == thread())
	{
		m_frame = std::move(frame);
		++m_frameCount;
		update();
		return;
	}
	{
		QMutexLocker lock(&m_mutex);
		m_pending = std::move(frame);
		if (m_scheduled) return;
		m_scheduled = true;
	}
	QMetaObject::invokeMethod(this, [this]
	{
		Frame next;
		{
			QMutexLocker lock(&m_mutex);
			next = std::move(m_pending);
			m_scheduled = false;
		}
		if (next.image.isNull()) return;
		m_frame = std::move(next);
		++m_frameCount;
		update();
	}, Qt::QueuedConnection);
}

void KRRPreviewVideo::paintEvent(QPaintEvent*)
{
	QPainter painter(this);
	const QColor background = property("immersivePreview").toBool() ? QColor(Qt::black) : QColor("#152133");
	const auto& image = m_frame.image;
	if (image.isNull())
	{
		painter.fillRect(rect(), background);
		return;
	}
	QRect source = m_frame.viewport.intersected(image.rect());
	if (source.isEmpty()) source = image.rect();
	const auto size = source.size().scaled(this->size(), Qt::KeepAspectRatio);
	const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
	painter.fillRect(0, 0, width(), target.top(), background);
	painter.fillRect(0, target.bottom() + 1, width(), height() - target.bottom() - 1, background);
	painter.fillRect(0, target.top(), target.left(), target.height(), background);
	painter.fillRect(target.right() + 1, target.top(), width() - target.right() - 1, target.height(), background);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	if (m_frame.bottomUp)
	{
		source.moveTop(image.height() - source.bottom() - 1);
		painter.translate(target.left(), target.bottom() + 1);
		painter.scale(1, -1);
		painter.drawImage(QRect(QPoint(), size), image, source);
	}
	else painter.drawImage(target, image, source);
}

void ChangePreviewRate(QMediaPlayer* player, qreal rate)
{
	if (rate <= 0 || qFuzzyCompare(rate, player->playbackRate())) return;
	player->setPlaybackRate(rate);
}
