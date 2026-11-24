#include "cameracapture.h"
#include "recordingconfig.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <QJsonObject>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
using Microsoft::WRL::ComPtr;
struct KRRCameraCapture::State
{
	std::thread worker;
	std::atomic<bool> stop{false};
	std::atomic<bool> running{false};
	std::mutex mutex;
	std::condition_variable changed;
	QImage image;
	quint64 sequence = 0;
	QString error;
	bool ready = false;
};
struct PendingSample
{
	std::mutex mutex;
	std::condition_variable changed;
	ComPtr<IMFSample> sample;
	HRESULT result = S_OK;
	DWORD flags = 0;
	bool ready = false, flushed = false, closing = false;
};
class ReaderCallback final : public IMFSourceReaderCallback
{
public:
	explicit ReaderCallback(std::shared_ptr<PendingSample> pending) : m_pending(std::move(pending)) {}
	STDMETHODIMP QueryInterface(REFIID iid, void** object) override
	{
		if (!object) return E_POINTER;
		*object = nullptr;
		if (iid != IID_IUnknown && iid != __uuidof(IMFSourceReaderCallback)) return E_NOINTERFACE;
		*object = static_cast<IMFSourceReaderCallback*>(this); AddRef(); return S_OK;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return ++m_references; }
	STDMETHODIMP_(ULONG) Release() override { ULONG count = --m_references; if (!count) delete this; return count; }
	STDMETHODIMP OnReadSample(HRESULT result, DWORD, DWORD flags, LONGLONG, IMFSample* sample) override
	{
		std::lock_guard<std::mutex> lock(m_pending->mutex);
		if (m_pending->closing) return S_OK;
		m_pending->result = result; m_pending->flags = flags; m_pending->sample = sample;
		m_pending->ready = true; m_pending->changed.notify_all(); return S_OK;
	}
	STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
	STDMETHODIMP OnFlush(DWORD) override
	{
		std::lock_guard<std::mutex> lock(m_pending->mutex); m_pending->flushed = true; m_pending->changed.notify_all(); return S_OK;
	}
private:
	std::atomic<ULONG> m_references{1};
	std::shared_ptr<PendingSample> m_pending;
};
static HRESULT Enumerate(IMFActivate*** devices, UINT32* count)
{
	ComPtr<IMFAttributes> attributes;
	HRESULT result = MFCreateAttributes(&attributes, 1);
	if (SUCCEEDED(result)) result = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	return SUCCEEDED(result) ? MFEnumDeviceSources(attributes.Get(), devices, count) : result;
}
static void FreeDevices(IMFActivate** devices, UINT32 count)
{
	for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
	CoTaskMemFree(devices);
}
KRRCameraCapture::KRRCameraCapture() : m_state(std::make_unique<State>()) {}
KRRCameraCapture::~KRRCameraCapture() { stop(); }
QJsonArray KRRCameraCapture::Devices()
{
	HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	QJsonArray result;
	if (SUCCEEDED(MFStartup(MF_VERSION)))
	{
		IMFActivate** devices = nullptr; UINT32 count = 0;
		if (SUCCEEDED(Enumerate(&devices, &count)))
		{
			for (UINT32 i = 0; i < count; ++i)
			{
				wchar_t* name = nullptr; UINT32 length = 0;
				devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length);
				result.append(QJsonObject{{"index", static_cast<int>(i)}, {"name", name ? QString::fromWCharArray(name) : "Camera"}});
				CoTaskMemFree(name);
			}
			FreeDevices(devices, count);
		}
		MFShutdown();
	}
	if (SUCCEEDED(com)) CoUninitialize();
	return result;
}
void KRRCameraCapture::startAsync(int index)
{
	stop();
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->stop.store(false);
		m_state->running.store(true);
		m_state->ready = false;
		m_state->error.clear();
		m_state->image = {};
		m_state->sequence = 0;
	}
	m_state->worker = std::thread([this, index]
	{
		HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		HRESULT result = MFStartup(MF_VERSION);
		QString stage = "MFStartup";
		bool mf = SUCCEEDED(result);
		IMFActivate** devices = nullptr; UINT32 count = 0;
		ComPtr<IMFMediaSource> source;
		ComPtr<IMFSourceReader> reader;
		if (SUCCEEDED(result)) { stage = "MFEnumDeviceSources"; result = Enumerate(&devices, &count); }
		if (SUCCEEDED(result)) { stage = "IMFActivate::ActivateObject"; result = index >= 0 && static_cast<UINT32>(index) < count ? devices[index]->ActivateObject(IID_PPV_ARGS(&source)) : E_INVALIDARG; }
		FreeDevices(devices, count);
		auto pending = std::make_shared<PendingSample>();
		ComPtr<IMFSourceReaderCallback> callback; callback.Attach(new ReaderCallback(pending));
		ComPtr<IMFAttributes> attributes;
		if (SUCCEEDED(result)) result = MFCreateAttributes(&attributes, 3);
		if (SUCCEEDED(result)) result = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
		if (SUCCEEDED(result)) result = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
		if (SUCCEEDED(result)) { stage = "MFCreateSourceReaderFromMediaSource"; result = MFCreateSourceReaderFromMediaSource(source.Get(), attributes.Get(), &reader); }
		ComPtr<IMFMediaType> type;
		if (SUCCEEDED(result)) result = MFCreateMediaType(&type);
		if (SUCCEEDED(result)) result = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		if (SUCCEEDED(result)) result = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
		if (SUCCEEDED(result)) { stage = "SetCurrentMediaType(RGB32)"; result = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get()); }
		if (SUCCEEDED(result)) result = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
		UINT32 width = 0, height = 0;
		if (SUCCEEDED(result)) result = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
		constexpr auto frameTimeout = std::chrono::seconds(6);
		auto frameDeadline = std::chrono::steady_clock::now() + frameTimeout;
		while (SUCCEEDED(result) && !m_state->stop.load())
		{
			if (std::chrono::steady_clock::now() >= frameDeadline)
			{
				stage = "waiting for a camera frame";
				result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
				break;
			}
			DWORD flags = 0; ComPtr<IMFSample> sample;
			stage = "IMFSourceReader::ReadSample(async)";
			{ std::lock_guard<std::mutex> lock(pending->mutex); pending->ready = false; pending->sample.Reset(); }
			result = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr);
			if (FAILED(result)) break;
			{
				std::unique_lock<std::mutex> lock(pending->mutex);
				while (!pending->ready && !m_state->stop.load() && std::chrono::steady_clock::now() < frameDeadline)
					pending->changed.wait_for(lock, std::chrono::milliseconds(100));
				if (m_state->stop.load()) break;
				if (!pending->ready)
				{
					stage = "waiting for a camera frame";
					result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
					break;
				}
				result = pending->result; flags = pending->flags; sample = pending->sample;
			}
			if (flags & MF_SOURCE_READERF_ENDOFSTREAM) result = MF_E_END_OF_STREAM;
			if (flags & MF_SOURCE_READERF_ERROR && SUCCEEDED(result)) result = E_FAIL;
			if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && SUCCEEDED(result))
			{
				stage = "GetCurrentMediaType";
				result = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
				if (SUCCEEDED(result)) result = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
			}
			if (FAILED(result)) break;
			if (!sample) continue;
			ComPtr<IMFMediaBuffer> buffer;
			result = sample->ConvertToContiguousBuffer(&buffer);
			if (FAILED(result)) break;
			BYTE* pixels = nullptr; DWORD length = 0;
			result = buffer->Lock(&pixels, nullptr, &length);
			if (SUCCEEDED(result))
			{
				if (width > 0 && height > 0 && length >= width * height * 4)
				{
					LONG stride = static_cast<LONG>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, width * 4));
					qint64 rowSize = stride < 0 ? -static_cast<qint64>(stride) : stride;
					if (rowSize < width * 4 || rowSize * (height - 1) + width * 4 > length) { buffer->Unlock(); result = MF_E_BUFFERTOOSMALL; break; }
					QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB32);
					for (UINT32 y = 0; y < height; ++y)
					{
						const BYTE* row = stride >= 0 ? pixels + static_cast<size_t>(y) * stride : pixels + static_cast<size_t>(height - y - 1) * (-stride);
						memcpy(image.scanLine(static_cast<int>(y)), row, width * 4);
					}
					std::lock_guard<std::mutex> lock(m_state->mutex);
					m_state->image = image; ++m_state->sequence; m_state->ready = true; m_state->changed.notify_all();
					frameDeadline = std::chrono::steady_clock::now() + frameTimeout;
				}
				buffer->Unlock();
			}
		}
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			if (!m_state->stop.load())
			{
				m_state->image = {};
				int integrity = ProcessIntegrityRid();
				QString hint = "Check connection, device use and Windows camera access.";
				if (result == E_ACCESSDENIED && integrity >= 0 && integrity < SECURITY_MANDATORY_MEDIUM_RID)
					hint = "Extract the entire ZIP into Downloads and open exe_cli/wpsrecorder.exe.";
				else if (IsContainerProcess()) hint = "This process runs in an AppContainer; camera capability access must be granted by Windows.";
				m_state->error = QString("Camera failed at %1 (0x%2). %3").arg(stage).arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0')).arg(hint);
			}
			m_state->ready = true; m_state->changed.notify_all();
		}
		// All COM device operations stay on this MTA thread. Async requests allow
		// stop to cancel pending reads without calling Shutdown from another apartment.
		{ std::lock_guard<std::mutex> lock(pending->mutex); pending->closing = true; pending->sample.Reset(); }
		if (reader && SUCCEEDED(reader->Flush(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS))))
		{
			std::unique_lock<std::mutex> lock(pending->mutex);
			pending->changed.wait_for(lock, std::chrono::seconds(2), [&] { return pending->flushed; });
		}
		reader.Reset();
		if (source) source->Shutdown();
		source.Reset(); type.Reset(); attributes.Reset(); callback.Reset();
		// Release the last retained sample while MF and this COM apartment still live.
		{ std::lock_guard<std::mutex> lock(pending->mutex); pending->sample.Reset(); }
		if (mf) MFShutdown();
		if (SUCCEEDED(com)) CoUninitialize();
		m_state->running.store(false);
	});
}
bool KRRCameraCapture::start(int index, QString& error)
{
	startAsync(index);
	std::unique_lock<std::mutex> lock(m_state->mutex);
	if (!m_state->changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_state->ready; })) error = "Camera initialization timed out";
	else error = m_state->error;
	bool success = !m_state->image.isNull() && error.isEmpty();
	lock.unlock();
	if (!success) stop();
	return success;
}
void KRRCameraCapture::stop()
{
	stopAsync();
	if (m_state->worker.joinable()) m_state->worker.join();
}
void KRRCameraCapture::stopAsync()
{
	m_state->stop.store(true);
}
bool KRRCameraCapture::isRunning() const
{
	return m_state->running.load();
}
QImage KRRCameraCapture::frame(QString& error, quint64* sequence)
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	if (sequence) *sequence = m_state->sequence;
	error = m_state->error;
	return m_state->image;
}
quint64 KRRCameraCapture::frameSequence() const
{
	std::lock_guard<std::mutex> lock(m_state->mutex); return m_state->sequence;
}
