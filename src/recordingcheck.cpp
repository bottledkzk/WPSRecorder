#include "recordingcheck.h"
#include "cameracapture.h"
#include "compositor.h"
#include "framecapture.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

static QSize PreviewSize(QSize size, const QSize& maximum)
{
	if (!maximum.isEmpty() && (size.width() > maximum.width() || size.height() > maximum.height()))
		size.scale(maximum, Qt::KeepAspectRatio);
	return size;
}

struct KRRLiveCapturePreview::Impl
{
	Impl(KRRRecordingConfig config, int intervalMs, QSize maximum, bool compose)
	{
		const auto size = PreviewSize(QSize(config.width, config.height), maximum);
		config.width = qMax(2, size.width());
		config.height = qMax(2, size.height());
		worker = std::thread([this, config, intervalMs, compose]() mutable
		{
			KRRFrameCapture capture;
			KRRCameraCapture camera;
			const bool cameraSource = compose && config.source == "camera";
			const QImage still = config.source == "image" ? QImage(config.image)
				.scaled(config.width, config.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation) : QImage{};
			auto due = std::chrono::steady_clock::now();
			std::chrono::steady_clock::time_point cameraRetry;
			std::optional<int> cameraDevice;
			QString lastCameraError;
			qint64 sequence = 0;
			std::unique_lock<std::mutex> lock(mutex);
			while (!stopping)
			{
				if (pendingComposition)
				{
					const auto& next = *pendingComposition;
					config.camera = next.camera;
					config.cameraEnabled = next.cameraEnabled;
					config.cameraMirror = next.cameraMirror;
					config.cameraShape = next.cameraShape;
					config.cameraRect = next.cameraRect;
					config.cameraBorder = next.cameraBorder;
					config.effects = next.effects;
					pendingComposition.reset();
				}
				lock.unlock();
				const bool needsCamera = cameraSource || (compose && config.cameraEnabled);
				if (cameraDevice && (!needsCamera || *cameraDevice != config.camera))
				{
					camera.stopAsync();
					cameraDevice.reset();
					lastCameraError.clear();
				}
				const auto now = std::chrono::steady_clock::now();
				if (needsCamera && !camera.isRunning() && (!cameraDevice || (config.camera >= 0 && now >= cameraRetry)))
				{
					camera.startAsync(config.camera);
					cameraDevice = config.camera;
					cameraRetry = now + std::chrono::seconds(3);
				}
				QString error, cameraError;
				const auto cameraFrame = cameraDevice ? camera.frame(cameraError) : QImage{};
				if (!cameraError.isEmpty()) lastCameraError = cameraError;
				else if (!cameraFrame.isNull()) lastCameraError.clear();
				QImage image;
				if (cameraSource && !cameraFrame.isNull())
				{
					image = cameraFrame.scaled(config.width, config.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
					if (config.cameraMirror) image = image.mirrored(true, false);
				}
				else if (cameraSource) error = lastCameraError;
				else if (config.source == "image")
				{
					image = still;
					if (image.isNull()) error = "Cannot decode the selected image";
				}
				else image = capture.capture(config, error);
				if (compose)
				{
					if (!cameraSource && config.cameraEnabled && !image.isNull())
						DrawCamera(image, cameraFrame, config);
					ApplyRecordingPreviewEffects(image, config);
				}
				auto diagnostics = capture.diagnostics();
				lock.lock();
				frame = {std::move(image), std::move(error),
					std::move(diagnostics), ++sequence, cameraFrame, lastCameraError};
				const auto interval = std::chrono::milliseconds(frame.image.isNull() ? qMax(100, intervalMs) : intervalMs);
				due = qMax(due + interval, std::chrono::steady_clock::now());
				wake.wait_until(lock, due, [this] { return stopping; });
			}
		});
	}

	~Impl()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		wake.notify_one();
		worker.join();
	}

	void updateComposition(const KRRRecordingConfig& config)
	{
		std::lock_guard<std::mutex> lock(mutex);
		pendingComposition = config;
	}

	Frame latest()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return frame;
	}

	std::mutex mutex;
	std::condition_variable wake;
	std::optional<KRRRecordingConfig> pendingComposition;
	Frame frame;
	bool stopping = false;
	std::thread worker;
};


KRRLiveCapturePreview::KRRLiveCapturePreview(const KRRRecordingConfig& config, int intervalMs, QSize maximum, bool compose)
	: d(std::make_unique<Impl>(config, intervalMs, maximum, compose))
{
}

KRRLiveCapturePreview::~KRRLiveCapturePreview() = default;
void KRRLiveCapturePreview::updateComposition(const KRRRecordingConfig& config) { d->updateComposition(config); }
KRRLiveCapturePreview::Frame KRRLiveCapturePreview::latest() { return d->latest(); }

QImage CaptureRecordingPreview(const KRRRecordingConfig& config, QJsonObject& captureInfo, QString& error)
{
	auto preview = config;
	const auto size = PreviewSize(QSize(config.width, config.height), QSize(2560, 1440));
	if (size.width() < 2 || size.height() < 2)
	{
		error = "Invalid capture rectangle";
		return {};
	}
	preview.width = size.width();
	preview.height = size.height();
	KRRFrameCapture capture;
	KRRCameraCapture camera;
	QImage cameraFrame;
	if (config.source == "camera" || config.cameraEnabled || config.cameraTrack)
	{
		if (!camera.start(config.camera, error)) return {};
		cameraFrame = camera.frame(error);
		if (cameraFrame.isNull()) return {};
	}
	QImage image;
	if (config.source == "camera")
	{
		image = cameraFrame.convertToFormat(QImage::Format_RGB32);
		if (config.cameraMirror) image = image.mirrored(true, false);
		if (image.size() != size)
			image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		captureInfo = {{"api", "Media Foundation"}};
	}
	else
	{
		image = capture.capture(preview, error);
		captureInfo = capture.diagnostics();
		if (!image.isNull() && config.cameraEnabled) DrawCamera(image, cameraFrame, preview);
	}
	ApplyRecordingPreviewEffects(image, config);
	return image;
}

void ApplyRecordingPreviewEffects(QImage& image, const KRRRecordingConfig& config)
{
	if (image.isNull() || config.effects.isEmpty()) return;
	const auto area = KRRFrameCapture::CaptureArea(config);
	POINT cursor{};
	GetCursorPos(&cursor);
	const QPointF pointer(
		area.width() > 0 ? (cursor.x - area.x()) / static_cast<double>(area.width()) : .5,
		area.height() > 0 ? (cursor.y - area.y()) / static_cast<double>(area.height()) : .5);
	ApplyEffects(image, config.effects, pointer);
}
