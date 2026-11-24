#include "previewchannel.h"
#include "recordingconfig.h"
#include "recordingpixels.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <windows.h>
namespace
{
constexpr DWORD Magic = 0x57505250, Version = 2;
constexpr int FrameWidth = 1920, FrameHeight = 1200, CameraWidth = 960, CameraHeight = 720;
constexpr size_t FrameBytes = FrameWidth * FrameHeight * 4, CameraBytes = CameraWidth * CameraHeight * 4;
QImage BoundedPreview(const QImage& image, int width, int height)
{
	const auto scaled = image.width() <= width && image.height() <= height ? image :
		image.scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	return ToneMapRecordingPreview(scaled).convertToFormat(QImage::Format_RGB32);
}
struct Header
{
	DWORD magic, version, active, width, height, cameraWidth, cameraHeight;
	qint64 sequence;
};
constexpr size_t MappingBytes = sizeof(Header) + FrameBytes + CameraBytes;
QString Key()
{
	return "Local\\WPSRecorderPreview-v2-" +
		   QString::fromLatin1(
			   QCryptographicHash::hash(ControlServerName().toUtf8(), QCryptographicHash::Sha256).toHex());
}
struct Mapping
{
	HANDLE memory = nullptr, mutex = nullptr;
	void *data = nullptr;
	~Mapping()
	{
		close();
	}
	void close()
	{
		if (data)
			UnmapViewOfFile(data);
		if (memory)
			CloseHandle(memory);
		if (mutex)
			CloseHandle(mutex);
		data = nullptr;
		memory = mutex = nullptr;
	}
	bool open(bool writer)
	{
		if (data)
			return true;
		auto name = Key().toStdWString(), lockName = (Key() + "-mutex").toStdWString();
		memory = writer ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
											 static_cast<DWORD>(MappingBytes), name.c_str())
						: OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
		mutex = writer ? CreateMutexW(nullptr, FALSE, lockName.c_str())
					   : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, lockName.c_str());
		if (memory && mutex)
			data = MapViewOfFile(memory, writer ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, MappingBytes);
		if (!data)
			close();
		return data != nullptr;
	}
	bool lock(DWORD timeout = 0)
	{
		DWORD result = WaitForSingleObject(mutex, timeout);
		return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
	}
};
} // namespace
struct KRRPreviewPublisher::Impl
{
	std::mutex mutex;
	std::condition_variable changed;
	bool exiting = false, active = false, pending = false, mirror = false;
	quint64 generation = 0;
	KRRPreviewFrame input, output;
	Mapping mapping;
	std::thread worker;
	~Impl()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			exiting = true;
		}
		changed.notify_one();
		if (worker.joinable()) worker.join();
	}
	void run()
	{
		auto next = std::chrono::steady_clock::now();
		std::unique_lock<std::mutex> lock(mutex);
		while (!exiting)
		{
			changed.wait(lock, [this] { return exiting || pending; });
			if (exiting)
				break;
			changed.wait_until(lock, next, [this] { return exiting || !active; });
			if (exiting)
				break;
			if (!pending || !active)
				continue;
			auto frame = input;
			bool flip = mirror;
			auto token = generation;
			pending = false;
			lock.unlock();
			frame.frame = BoundedPreview(frame.frame, FrameWidth, FrameHeight);
			if (!frame.camera.isNull())
			{
				frame.camera = BoundedPreview(frame.camera, CameraWidth, CameraHeight);
				if (flip)
					frame.camera = frame.camera.mirrored(true, false);
			}
			lock.lock();
			if (active && generation == token)
			{
				output = frame;
				if (mapping.open(true) && mapping.lock(2))
				{
					Header header{Magic,
								  Version,
								  1,
								  static_cast<DWORD>(frame.frame.width()),
								  static_cast<DWORD>(frame.frame.height()),
								  static_cast<DWORD>(frame.camera.width()),
								  static_cast<DWORD>(frame.camera.height()),
								  frame.sequence};
					std::memcpy(mapping.data, &header, sizeof(header));
					auto bytes = static_cast<char *>(mapping.data) + sizeof(Header);
					std::memcpy(bytes, frame.frame.constBits(), static_cast<size_t>(frame.frame.sizeInBytes()));
					if (!frame.camera.isNull())
						std::memcpy(bytes + FrameBytes, frame.camera.constBits(),
									static_cast<size_t>(frame.camera.sizeInBytes()));
					ReleaseMutex(mapping.mutex);
				}
			}
			next = std::chrono::steady_clock::now() + std::chrono::milliseconds(32);
		}
	}
};
KRRPreviewPublisher::KRRPreviewPublisher() : d(std::make_unique<Impl>())
{
}
KRRPreviewPublisher::~KRRPreviewPublisher() = default;
void KRRPreviewPublisher::reset()
{
	clear();
	std::lock_guard<std::mutex> lock(d->mutex);
	d->active = true;
}
void KRRPreviewPublisher::clear()
{
	std::lock_guard<std::mutex> lock(d->mutex);
	d->active = d->pending = false;
	++d->generation;
	d->input = {};
	d->output = {};
	if (d->mapping.data && d->mapping.lock(2))
	{
		std::memset(d->mapping.data, 0, sizeof(Header));
		ReleaseMutex(d->mapping.mutex);
	}
	d->changed.notify_one();
}
void KRRPreviewPublisher::submit(const QImage &frame, const QImage &camera, bool mirror, qint64 sequence)
{
	std::lock_guard<std::mutex> lock(d->mutex);
	if (!d->active)
		return;
	if (!d->worker.joinable()) d->worker = std::thread([impl = d.get()] { impl->run(); });
	d->input = {frame, camera, sequence};
	d->mirror = mirror;
	d->pending = true;
	d->changed.notify_one();
}
KRRPreviewFrame KRRPreviewPublisher::snapshot()
{
	std::lock_guard<std::mutex> lock(d->mutex);
	return d->output;
}
struct KRRPreviewReader::Impl
{
	Mapping mapping;
	QElapsedTimer attempt;
	KRRPreviewFrame latest;
};
KRRPreviewReader::KRRPreviewReader() : d(std::make_unique<Impl>())
{
}
KRRPreviewReader::~KRRPreviewReader() = default;
void KRRPreviewReader::reset()
{
	d->mapping.close();
	d->attempt.invalidate();
	d->latest = {};
}
bool KRRPreviewReader::read(KRRPreviewFrame &frame)
{
	if (!d->mapping.data)
	{
		if (d->attempt.isValid() && d->attempt.elapsed() < 250)
			return false;
		d->attempt.start();
		if (!d->mapping.open(false))
			return false;
	}
	if (!d->mapping.lock())
		return false; // Never block the GUI behind the publisher.
	Header header{};
	std::memcpy(&header, d->mapping.data, sizeof(header));
	bool valid = header.magic == Magic && header.version == Version && header.active && header.width > 0 &&
				 header.width <= FrameWidth && header.height > 0 && header.height <= FrameHeight &&
				 header.cameraWidth <= CameraWidth && header.cameraHeight <= CameraHeight;
	if (valid)
	{
		if (d->latest.frame.isNull() || d->latest.sequence != header.sequence)
		{
			auto bytes = static_cast<uchar *>(d->mapping.data) + sizeof(Header);
			d->latest.frame = QImage(bytes, header.width, header.height, QImage::Format_RGB32).copy();
			d->latest.camera = header.cameraWidth && header.cameraHeight
				? QImage(bytes + FrameBytes, header.cameraWidth, header.cameraHeight, QImage::Format_RGB32).copy()
				: QImage{};
			d->latest.sequence = header.sequence;
		}
		frame = d->latest;
	}
	else d->latest = {};
	ReleaseMutex(d->mapping.mutex);
	return valid;
}
