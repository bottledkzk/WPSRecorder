#include "audiocapture.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <QFile>
#include <algorithm>
#include <cmath>
#include <vector>

using Microsoft::WRL::ComPtr;

QVector<KRRAudioEndpoint> RecordingAudioEndpoints()
{
	QVector<KRRAudioEndpoint> result;
	const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	{
		ComPtr<IMMDeviceEnumerator> enumerator;
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
			for (const auto flow : {eCapture, eRender})
			{
				ComPtr<IMMDeviceCollection> devices;
				UINT count = 0;
				if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices)) ||
					FAILED(devices->GetCount(&count))) continue;
				for (UINT i = 0; i < count; ++i)
				{
					ComPtr<IMMDevice> device;
					ComPtr<IPropertyStore> store;
					LPWSTR id = nullptr;
					PROPVARIANT name{};
					if (SUCCEEDED(devices->Item(i, &device)) && SUCCEEDED(device->GetId(&id)) &&
						SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
						SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
						result.append({QString::fromWCharArray(id), QString::fromWCharArray(name.pwszVal), flow == eRender});
					CoTaskMemFree(id);
					PropVariantClear(&name);
				}
			}
	}
	if (SUCCEEDED(initialized)) CoUninitialize();
	return result;
}

KRRAudioCapture::KRRAudioCapture(bool loopback, KRRRecordingClock& clock, const QString& endpoint)
	: m_loopback(loopback), m_clock(&clock), m_endpoint(endpoint)
{
}
KRRAudioCapture::KRRAudioCapture(bool loopback, const QString& endpoint) : m_loopback(loopback), m_endpoint(endpoint) {}
KRRAudioCapture::~KRRAudioCapture() { stop(); }
std::future<QString> KRRAudioCapture::launch(const QString& path)
{
	std::promise<QString> initialized;
	auto future = initialized.get_future();
	m_thread = std::thread([this, path, initialized = std::move(initialized)]() mutable
	{
		HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(result))
		{
			const QString error = "Could not initialize COM audio thread";
			{ std::lock_guard<std::mutex> lock(m_mutex); m_error = error; }
			initialized.set_value(error);
			return;
		}
		captureLoop(path, initialized);
		CoUninitialize();
	});
	return future;
}
bool KRRAudioCapture::start(const QString& path, QString& error)
{
	error = launch(path).get();
	return error.isEmpty();
}
void KRRAudioCapture::startMeter() { launch({}); }
void KRRAudioCapture::measure(const float* pcm, qint64 count, float gain)
{
	float peak = 0;
	for (qint64 i = 0; i < count; ++i) peak = qMax(peak, std::abs(pcm[i]));
	const int value = qRound(qMin(1.f, peak * gain) * 10000);
	int previous = m_meterPeak.load();
	while (previous < value && !m_meterPeak.compare_exchange_weak(previous, value)) {}
}
void KRRAudioCapture::stop()
{
	m_stop.store(true);
	if (m_thread.joinable()) m_thread.join();
}
QString KRRAudioCapture::error() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_error;
}
QString KRRAudioCapture::deviceName() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_deviceName;
}
void KRRAudioCapture::captureLoop(const QString& path, std::promise<QString>& initialized)
{
	const bool meter = path.isEmpty();
	auto fail = [this, &initialized](const QString& error)
	{
		{ std::lock_guard<std::mutex> lock(m_mutex); m_error = error; }
		initialized.set_value(error);
	};
	const auto unavailable = [this](HRESULT result)
	{
		return QString(m_loopback ? "System audio unavailable: " : "Microphone unavailable: ") +
			QString("HRESULT 0x%1").arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
	};
	ComPtr<IMMDeviceEnumerator> enumerator;
	ComPtr<IMMDevice> device;
	ComPtr<IAudioClient> client;
	ComPtr<IAudioCaptureClient> capture;
	HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
	if (SUCCEEDED(result))
		result = m_endpoint.isEmpty() ? enumerator->GetDefaultAudioEndpoint(m_loopback ? eRender : eCapture, eConsole, &device)
			: enumerator->GetDevice(reinterpret_cast<LPCWSTR>(m_endpoint.utf16()), &device);
	if (meter && SUCCEEDED(result))
	{
		ComPtr<IPropertyStore> properties;
		PROPVARIANT name{};
		if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
			SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_deviceName = QString::fromWCharArray(name.pwszVal);
		}
		PropVariantClear(&name);
	}
	if (SUCCEEDED(result)) result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
	WAVEFORMATEX format{};
	format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
	format.nChannels = 2;
	format.nSamplesPerSec = 48000;
	format.wBitsPerSample = 32;
	format.nBlockAlign = 8;
	format.nAvgBytesPerSec = 384000;
	DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
	if (m_loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
	if (SUCCEEDED(result)) result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, &format, nullptr);
	if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&capture));
	QFile output(path);
	if (FAILED(result))
	{
		fail(unavailable(result));
		return;
	}
	if (!meter && !output.open(QIODevice::WriteOnly))
	{
		fail("Could not write audio data; check free disk space");
		return;
	}
	result = client->Start();
	if (FAILED(result))
	{
		fail(unavailable(result));
		return;
	}
	initialized.set_value({});
	if (meter)
	{
		m_meterReady.store(true);
		while (!m_stop.load())
		{
			UINT32 frames = 0;
			result = capture->GetNextPacketSize(&frames);
			if (FAILED(result)) break;
			if (!frames) { Sleep(10); continue; }
			BYTE* samples = nullptr;
			DWORD packetFlags = 0;
			result = capture->GetBuffer(&samples, &frames, &packetFlags, nullptr, nullptr);
			if (FAILED(result)) break;
			if (samples && !(packetFlags & AUDCLNT_BUFFERFLAGS_SILENT))
				measure(reinterpret_cast<const float*>(samples), qint64(frames) * 2, m_gain.load());
			capture->ReleaseBuffer(frames);
		}
		client->Stop();
		if (FAILED(result))
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_error = "Audio device disconnected during capture";
		}
		return;
	}
	qint64 writtenFrames = 0;
	const std::vector<float> silence(8192, 0.0f);
	std::vector<float> adjusted;
	bool writeFailed = false;
	auto writeSilence = [&](qint64 frames)
	{
		while (frames > 0 && !writeFailed)
		{
			qint64 chunk = std::min<qint64>(frames, 4096);
			writeFailed = output.write(reinterpret_cast<const char*>(silence.data()), chunk * 8) != chunk * 8;
			frames -= chunk;
			writtenFrames += chunk;
		}
	};
	while (!m_stop.load() && !writeFailed)
	{
		UINT32 packetFrames = 0;
		result = capture->GetNextPacketSize(&packetFrames);
		if (FAILED(result)) break;
		if (!packetFrames)
		{
			Sleep(3);
			continue;
		}
		BYTE* samples = nullptr;
		UINT32 frames = 0;
		DWORD packetFlags = 0;
		UINT64 devicePosition = 0, qpcPosition = 0;
		result = capture->GetBuffer(&samples, &frames, &packetFlags, &devicePosition, &qpcPosition);
		if (FAILED(result)) break;
		double timestamp = static_cast<double>(qpcPosition) / 10000000.0;
		if (packetFlags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
			timestamp = KRRRecordingClock::Now() - static_cast<double>(frames) / 48000.0;
		double activeTime = m_clock->activeTimeAt(timestamp);
		if (activeTime >= 0)
		{
			qint64 target = static_cast<qint64>(std::llround(activeTime * 48000));
			if (target > writtenFrames) writeSilence(target - writtenFrames);
			qint64 skip = std::min<qint64>(frames, std::max<qint64>(0, writtenFrames - target));
			qint64 count = static_cast<qint64>(frames) - skip;
			const float gain = m_gain.load();
			if (!samples || (packetFlags & AUDCLNT_BUFFERFLAGS_SILENT) || m_muted.load() || gain == 0)
				writeSilence(count);
			else if (count > 0)
			{
				const float* pcm = reinterpret_cast<const float*>(samples) + skip * 2;
				measure(pcm, count * 2, gain);
				if (gain != 1.f)
				{
					adjusted.resize(static_cast<size_t>(count * 2));
					for (size_t i = 0; i < adjusted.size(); ++i) adjusted[i] = qBound(-1.f, pcm[i] * gain, 1.f);
					pcm = adjusted.data();
				}
				writeFailed = output.write(reinterpret_cast<const char*>(pcm), count * 8) != count * 8;
				writtenFrames += count;
			}
		}
		capture->ReleaseBuffer(frames);
	}
	client->Stop();
	qint64 finalFrames = static_cast<qint64>(std::ceil(m_clock->elapsed() * 48000));
	writeSilence(std::max<qint64>(0, finalFrames - writtenFrames));
	if (!output.flush()) writeFailed = true;
	if (FAILED(result) || writeFailed)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_error = writeFailed ? "Could not write audio data; check free disk space" : "Audio device disconnected during capture";
	}
}
