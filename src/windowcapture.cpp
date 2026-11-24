#include "windowcapture.h"
#include "recordingconfig.h"
#include "recordingtiming.h"
#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>
#include <d3d11.h>
#include <dwmapi.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

namespace
{
using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::SizeInt32;

constexpr auto PixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr int FirstFrameTimeoutMs = 350;
constexpr int CompatTimeoutMs = 60;
constexpr unsigned MaxWindowSessions = 6;
std::atomic_uint windowSessions = 0;

QRect FromRect(const RECT& rect)
{
	return {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
}

bool ValidCaptureSize(const QRect& bounds)
{
	return bounds.width() >= 2 && bounds.height() >= 2 && bounds.width() <= 16384 && bounds.height() <= 16384;
}

struct CompatibilitySurface
{
	HDC dc = CreateCompatibleDC(nullptr);
	HBITMAP bitmap = nullptr;
	HGDIOBJ previous = nullptr;
	void* pixels = nullptr;
	QSize size;

	~CompatibilitySurface()
	{
		if (previous) SelectObject(dc, previous);
		if (bitmap) DeleteObject(bitmap);
		if (dc) DeleteDC(dc);
	}

	bool resize(QSize requested)
	{
		if (!dc) return false;
		if (requested == size) return true;
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = requested.width();
		info.bmiHeader.biHeight = -requested.height();
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void* newPixels = nullptr;
		const auto newBitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &newPixels, nullptr, 0);
		if (!newBitmap) return false;
		const auto oldBitmap = SelectObject(dc, newBitmap);
		if (!oldBitmap || oldBitmap == HGDI_ERROR)
		{
			DeleteObject(newBitmap);
			return false;
		}
		if (bitmap) DeleteObject(bitmap);
		else previous = oldBitmap;
		bitmap = newBitmap;
		pixels = newPixels;
		size = requested;
		return true;
	}

	QImage capture(HWND window, const QRect& bounds)
	{
		if (!resize(bounds.size())) return {};
		// WM_PRINT can report success without painting. Clear old source pixels before asking the target.
		PatBlt(dc, 0, 0, size.width(), size.height(), BLACKNESS);
		GdiFlush();
		DWORD_PTR result = 0;
		if (!SendMessageTimeoutW(window, WM_PRINT, reinterpret_cast<WPARAM>(dc),
			PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_CHILDREN | PRF_CHECKVISIBLE,
			SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, CompatTimeoutMs, &result)) return {};
		GdiFlush();
		const auto* rgb = static_cast<const QRgb*>(pixels);
		const auto pixelCount = static_cast<size_t>(size.width()) * size.height();
		if (std::none_of(rgb, rgb + pixelCount, [](QRgb pixel) { return pixel & 0x00ffffff; })) return {};
		return QImage(static_cast<uchar*>(pixels), size.width(), size.height(), QImage::Format_RGB32).copy();
	}
};
}

struct KRRWindowCapture::State
{
	HWND window = nullptr;
	DWORD process = 0, thread = 0;
	HRESULT modernError = S_OK;
	const char* modernStage = "initialize";
	bool apartment = false;
	bool sessionSlot = false;
	CO_MTA_USAGE_COOKIE mta = nullptr;
	bool systemCursor = false;
	bool requestedCursor = false;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11Texture2D> staging;
	IDirect3DDevice directDevice{nullptr};
	GraphicsCaptureItem item{nullptr};
	GraphicsCaptureSession session{nullptr};
	Direct3D11CaptureFramePool pool{nullptr};
	std::shared_ptr<std::atomic_bool> closed = std::make_shared<std::atomic_bool>(false);
	GraphicsCaptureItem::Closed_revoker closedEvent;
	SizeInt32 poolSize{};
	QImage lastFrame;
	QRect lastBounds;
	std::unique_ptr<CompatibilitySurface> compatibility;
	inline static std::mutex pendingMutex;
	inline static std::array<State*, MaxWindowSessions> pending{};

	~State()
	{
		stopModern();
		if (apartment) RoUninitialize();
		if (mta) CoDecrementMTAUsage(mta);
		if (sessionSlot) windowSessions.fetch_sub(1);
	}

	static bool queueRetirement(State* state)
	{
		return TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE instance, void* context)
		{
			CallbackMayRunLong(instance);
			const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
			delete static_cast<State*>(context);
			if (SUCCEEDED(initialized)) RoUninitialize();
		}, state, nullptr);
	}

	static void retire(std::unique_ptr<State> state)
	{
		if (!state || (!state->session && !state->pool)) return;
		// Closing an agile WGC session can block in Windows after its target is terminated.
		// Keep that call off the preview and recording workers, including encoder finalization.
		const bool releaseApartment = std::exchange(state->apartment, false);
		State* retiring = state.release();
		if (!queueRetirement(retiring))
		{
			std::lock_guard<std::mutex> guard(pendingMutex);
			// Every retained state owns one of the bounded session slots.
			*std::find(pending.begin(), pending.end(), nullptr) = retiring;
		}
		if (releaseApartment) RoUninitialize();
	}

	static std::unique_ptr<State> useCompatibility(std::unique_ptr<State> previous)
	{
		auto fallback = std::make_unique<State>();
		fallback->window = previous->window;
		fallback->process = previous->process;
		fallback->thread = previous->thread;
		fallback->modernError = previous->modernError;
		fallback->modernStage = previous->modernStage;
		fallback->requestedCursor = previous->requestedCursor;
		fallback->closed = previous->closed;
		retire(std::move(previous));
		return fallback;
	}

	void stopModern()
	{
		closedEvent.revoke();
		try { if (session) session.Close(); } catch (const winrt::hresult_error&) {}
		try { if (pool) pool.Close(); } catch (const winrt::hresult_error&) {}
		session = nullptr;
		pool = nullptr;
		item = nullptr;
		directDevice = nullptr;
		staging.Reset();
		context.Reset();
		device.Reset();
		lastFrame = {};
	}

	void startModern(bool cursor)
	{
		{
			std::lock_guard<std::mutex> guard(pendingMutex);
			for (auto& state : pending) if (state && queueRetirement(state)) state = nullptr;
		}
		if (windowSessions.fetch_add(1) >= MaxWindowSessions)
		{
			windowSessions.fetch_sub(1);
			modernStage = "session_limit";
			winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_BUSY));
		}
		sessionSlot = true;
		const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
		apartment = SUCCEEDED(initialized);
		if (!apartment && initialized != RPC_E_CHANGED_MODE) winrt::check_hresult(initialized);
		winrt::check_hresult(CoIncrementMTAUsage(&mta));
		modernStage = "capture_support";
		if (!GraphicsCaptureSession::IsSupported()) winrt::throw_hresult(E_NOTIMPL);
		modernStage = "create_target";
		const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
		winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
		poolSize = item.Size();
		if (poolSize.Width < 2 || poolSize.Height < 2) winrt::throw_hresult(E_INVALIDARG);
		closedEvent = item.Closed(winrt::auto_revoke, [flag = closed](auto const&, auto const&) { flag->store(true); });
		modernStage = "create_device";
		winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
		ComPtr<IDXGIDevice> dxgiDevice;
		winrt::check_hresult(device.As(&dxgiDevice));
		winrt::com_ptr<::IInspectable> surfaceDevice;
		winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), surfaceDevice.put()));
		directDevice = surfaceDevice.as<IDirect3DDevice>();
		modernStage = "create_session";
		pool = Direct3D11CaptureFramePool::CreateFreeThreaded(directDevice, PixelFormat, 2, poolSize);
		session = pool.CreateCaptureSession(item);
		if (!winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
			L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
		{
			// Older sessions always paint the system cursor. Use the compatibility backend when it must be hidden.
			if (!cursor) winrt::throw_hresult(E_NOTIMPL);
		}
		else session.IsCursorCaptureEnabled(cursor);
		systemCursor = cursor;
		modernStage = "start_session";
		session.StartCapture();
		modernStage = "read_frame";
	}

	QImage readModern(KRRRecordingTimings* timings)
	{
		QElapsedTimer deadline;
		deadline.start();
		do
		{
			if (closed->load() || !IsWindow(window) || IsIconic(window)) return {};
			Direct3D11CaptureFrame frame{nullptr};
			{
				KRRStageTimer acquire(timings, KRRRecordingTimings::Acquire);
				frame = pool.TryGetNextFrame();
				if (frame)
					if (const auto next = pool.TryGetNextFrame()) frame = next;
			}
			if (frame)
			{
				const auto size = frame.ContentSize();
				const bool resized = poolSize.Width != size.Width || poolSize.Height != size.Height;
				if (size.Width >= 2 && size.Height >= 2 && size.Width <= poolSize.Width && size.Height <= poolSize.Height)
					lastFrame = readPixels(frame, size, timings);
				if (resized && size.Width >= 2 && size.Height >= 2)
				{
					frame.Close();
					pool.Recreate(directDevice, PixelFormat, 2, size);
					poolSize = size;
				}
			}
			if (!lastFrame.isNull()) return lastFrame;
			Sleep(2);
		} while (deadline.elapsed() < FirstFrameTimeoutMs);
		return {};
	}

	QImage readPixels(const Direct3D11CaptureFrame& frame, const SizeInt32& size, KRRRecordingTimings* timings)
	{
		KRRStageTimer readback(timings, KRRRecordingTimings::Readback);
		const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
		ComPtr<ID3D11Texture2D> source;
		winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
		D3D11_TEXTURE2D_DESC desc{};
		if (staging) staging->GetDesc(&desc);
		if (desc.Width != static_cast<UINT>(size.Width) || desc.Height != static_cast<UINT>(size.Height))
		{
			desc = {static_cast<UINT>(size.Width), static_cast<UINT>(size.Height), 1, 1,
				DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
			staging.Reset();
			winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &staging));
		}
		QImage image(size.Width, size.Height, QImage::Format_RGB32);
		if (image.isNull()) winrt::throw_hresult(E_OUTOFMEMORY);
		const D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
		context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
		for (int y = 0; y < size.Height; ++y)
			std::memcpy(image.scanLine(y), static_cast<const uchar*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
				static_cast<size_t>(size.Width) * 4);
		context->Unmap(staging.Get(), 0);
		return image;
	}
};

KRRWindowCapture::KRRWindowCapture() = default;
KRRWindowCapture::~KRRWindowCapture() { reset(); }

void KRRWindowCapture::reset()
{
	State::retire(std::move(m_state));
	m_info = {};
}

QRect KRRWindowCapture::Bounds(HWND window)
{
	RECT rect{};
	if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
	{
		const auto bounds = FromRect(rect);
		if (ValidCaptureSize(bounds)) return bounds;
	}
	return GetWindowRect(window, &rect) ? FromRect(rect) : QRect{};
}

QImage KRRWindowCapture::capture(HWND window, QRect& bounds, bool cursor, QString& error, KRRRecordingTimings* timings)
{
	const auto fail = [&](const char* status, const char* message) -> QImage
	{
		m_info = {{"api", "window"}, {"source_status", status}};
		if (m_state && FAILED(m_state->modernError))
		{
			m_info["wgc_stage"] = m_state->modernStage;
			m_info["wgc_hresult"] = QString::number(static_cast<quint32>(m_state->modernError), 16);
		}
		State::retire(std::move(m_state));
		error = QString::fromLatin1(message);
		return {};
	};
	const auto validate = [&]() -> QString
	{
		DWORD process = 0;
		const DWORD thread = GetWindowThreadProcessId(window, &process);
		if (!thread || !IsWindowVisible(window) || (m_state && m_state->window == window &&
			(m_state->thread != thread || m_state->process != process || m_state->closed->load()))) return "window_closed";
		if (IsIconic(window)) return "window_minimized";
		DWORD affinity = WDA_NONE;
		if (GetWindowDisplayAffinity(window, &affinity) && affinity != WDA_NONE) return "window_protected";
		return {};
	};
	const auto sourceFailure = [&](const QString& status) -> QImage
	{
		if (status == "window_protected") return fail("window_protected", "This window does not allow screen capture");
		if (status == "window_minimized") return fail("window_minimized", "Capture window was minimized; recording has stopped");
		return fail("window_closed", "Capture window was closed; recording has stopped");
	};
	if (m_state && m_state->window != window) reset();
	if (const auto status = validate(); !status.isEmpty()) return sourceFailure(status);
	bounds = Bounds(window);
	if (!ValidCaptureSize(bounds))
	{
		if (const auto status = validate(); !status.isEmpty()) return sourceFailure(status);
		// DWM can temporarily clear the rectangle while its window is resizing or closing.
		if (m_state && !m_state->lastFrame.isNull()) bounds = m_state->lastBounds;
		if (!ValidCaptureSize(bounds)) return fail("window_unavailable", "Invalid capture rectangle");
	}
	if (m_state && m_state->requestedCursor != cursor) reset();
	if (!m_state)
	{
		m_state = std::make_unique<State>();
		m_state->window = window;
		m_state->thread = GetWindowThreadProcessId(window, &m_state->process);
		m_state->requestedCursor = cursor;
		try { m_state->startModern(cursor); }
		catch (const winrt::hresult_error& failure)
		{
			m_state->modernError = failure.code();
			m_state = State::useCompatibility(std::move(m_state));
		}
	}
	if (m_state->pool)
	{
		try
		{
			const auto image = m_state->readModern(timings);
			if (const auto status = validate(); !status.isEmpty()) return sourceFailure(status);
			if (!image.isNull())
			{
				m_state->lastBounds = bounds;
				m_info = {{"api", "Windows Graphics Capture"}, {"native_cursor", m_state->systemCursor}};
				return image;
			}
			m_state->modernError = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
		}
		catch (const winrt::hresult_error& failure) { m_state->modernError = failure.code(); }
		m_state = State::useCompatibility(std::move(m_state));
	}
	if (const auto status = validate(); !status.isEmpty()) return sourceFailure(status);
	if (m_state->modernError == E_ACCESSDENIED || m_state->modernError == HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY))
	{
		const auto integrity = ProcessIntegrityRid();
		if (integrity >= 0 && integrity < SECURITY_MANDATORY_MEDIUM_RID)
			return fail("restricted_process", "Windows restricts window capture from this folder. Extract the entire ZIP into Downloads and open exe_cli/wpsrecorder.exe.");
		return fail("window_access_denied", "Windows denied permission to capture this window");
	}
	RECT windowRect{};
	if (!GetWindowRect(window, &windowRect)) return sourceFailure("window_closed");
	bounds = FromRect(windowRect);
	if (!ValidCaptureSize(bounds)) return fail("window_unavailable", "Invalid capture rectangle");
	if (!m_state->compatibility) m_state->compatibility = std::make_unique<CompatibilitySurface>();
	const auto image = m_state->compatibility->capture(window, bounds);
	if (const auto status = validate(); !status.isEmpty()) return sourceFailure(status);
	m_info = {{"api", "WM_PRINT"}, {"wgc_stage", m_state->modernStage},
		{"wgc_hresult", QString::number(static_cast<quint32>(m_state->modernError), 16)}};
	if (image.isNull())
	{
		m_info["source_status"] = "window_unavailable";
		error = "Windows could not capture the selected window";
	}
	return image;
}
