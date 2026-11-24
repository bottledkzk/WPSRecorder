#include "desktopcapture.h"
#include "desktopcolor.h"
#include "recordingconfig.h"
#include "recordingtiming.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QTransform>
#include <cmath>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
struct KRRDesktopCapture::State
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGIOutputDuplication> duplication;
	ComPtr<ID3D11Texture2D> staging;
	ComPtr<IDXGIOutput6> outputMetadata;
	DXGI_FORMAT nativeFormat = DXGI_FORMAT_UNKNOWN;
	QRect bounds;
	QImage cached;
	DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
	KRRDesktopColor color;
	QElapsedTimer retry, whiteRefresh, failure;
	QString deviceName;
	double white = 80;
	bool hdr = false, whiteKnown = false, firstAcquire = true;
	QJsonObject info;
	QRect requested;
	QSize size;
	QString colorMode = "auto";
	double exposure = 0, customWhite = 0;
};
KRRDesktopCapture::KRRDesktopCapture() : m_state(std::make_unique<State>())
{
}
KRRDesktopCapture::~KRRDesktopCapture() = default;
bool KRRDesktopCapture::initialize(const QRect &area)
{
	m_state->retry.start();
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return false;
	for (UINT adapterIndex = 0;; ++adapterIndex)
	{
		ComPtr<IDXGIAdapter1> adapter;
		if (FAILED(factory->EnumAdapters1(adapterIndex, &adapter)))
			break;
		for (UINT outputIndex = 0;; ++outputIndex)
		{
			ComPtr<IDXGIOutput> output;
			if (FAILED(adapter->EnumOutputs(outputIndex, &output)))
				break;
			DXGI_OUTPUT_DESC description{};
			if (FAILED(output->GetDesc(&description)) || !description.AttachedToDesktop)
				continue;
			RECT rectangle = description.DesktopCoordinates;
			if (!QRect(rectangle.left, rectangle.top, rectangle.right - rectangle.left,
					   rectangle.bottom - rectangle.top)
					 .contains(area))
				continue;
			D3D_FEATURE_LEVEL level{};
			if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
										 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
										 &m_state->device, &level, &m_state->context)))
				continue;
			ComPtr<IDXGIOutput1> output1;
			if (FAILED(output.As(&output1)))
				continue;
			auto &output6 = m_state->outputMetadata;
			output6.Reset();
			DXGI_OUTPUT_DESC1 color{};
			m_state->hdr = SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&color)) &&
						   color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
			ComPtr<IDXGIOutput5> output5;
			const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
			HRESULT hr = m_state->hdr && SUCCEEDED(output.As(&output5))
				? output5->DuplicateOutput1(m_state->device.Get(), 0, 2, formats, &m_state->duplication)
				: E_NOINTERFACE;
			if (FAILED(hr))
				hr = output1->DuplicateOutput(m_state->device.Get(), &m_state->duplication);
			if (FAILED(hr))
				continue;
			m_state->deviceName = QString::fromWCharArray(description.DeviceName);
			m_state->firstAcquire = true;
			m_state->white = ScreenSdrWhiteNits(m_state->deviceName, &m_state->whiteKnown);
			m_state->whiteRefresh.start();
			m_state->info = {{"api", "DXGI"},
							 {"hdr_active", m_state->hdr},
							 {"system_sdr_white_nits", m_state->white},
							 {"white_detected", m_state->whiteKnown}};
			m_state->staging.Reset();
			m_state->bounds = QRect(rectangle.left, rectangle.top, rectangle.right - rectangle.left,
									rectangle.bottom - rectangle.top);
			m_state->rotation = description.Rotation;
			return true;
		}
	}
	return false;
}
QImage KRRDesktopCapture::capture(const QRect &area, const QSize &outputSize, const KRRRecordingConfig &config,
									 KRRRecordingTimings* timings)
{
	if (m_state->requested != area || m_state->size != outputSize)
	{
		m_state = std::make_unique<State>();
		m_state->requested = area;
		m_state->size = outputSize;
	}
	if (m_state->colorMode != config.screenColor || m_state->exposure != config.screenExposureEV ||
		m_state->customWhite != config.screenWhiteNits)
	{
		m_state->colorMode = config.screenColor;
		m_state->exposure = config.screenExposureEV;
		m_state->customWhite = config.screenWhiteNits;
		m_state->duplication.Reset();
		m_state->retry.invalidate();
	}
	if (!m_state->duplication)
	{
		if (m_state->retry.isValid() && m_state->retry.elapsed() < 500)
			return m_state->failure.isValid() && m_state->failure.elapsed() > 1500 ? QImage{} : m_state->cached;
		if (!initialize(area))
			return m_state->failure.isValid() && m_state->failure.elapsed() > 1500 ? QImage{} : m_state->cached;
	}
	if (!m_state->bounds.contains(area))
		return {};
	DXGI_OUTDUPL_FRAME_INFO information{};
	ComPtr<IDXGIResource> resource;
	KRRStageTimer acquire(timings, KRRRecordingTimings::Acquire);
	const UINT wait = m_state->firstAcquire && m_state->cached.isNull() ? 50 : 0;
	m_state->firstAcquire = false;
	HRESULT result = m_state->duplication->AcquireNextFrame(wait, &information, &resource);
	acquire.finish();
	if (result == DXGI_ERROR_WAIT_TIMEOUT)
		return m_state->failure.isValid() && m_state->failure.elapsed() > 1500 ? QImage{} : m_state->cached;
	if (FAILED(result))
	{
		m_state->duplication.Reset();
		m_state->retry.start();
		m_state->info["reconnecting"] = true;
		if (!m_state->failure.isValid())
			m_state->failure.start();
		return m_state->cached;
	}
	if (!information.LastPresentTime.QuadPart)
	{
		m_state->duplication->ReleaseFrame();
		if (m_state->cached.isNull()) m_state->info["pointer_only"] = true;
		return m_state->failure.isValid() && m_state->failure.elapsed() > 1500 ? QImage{} : m_state->cached;
	}
	m_state->info.remove("pointer_only");
	if (information.ProtectedContentMaskedOut) m_state->info["protected_content_masked"] = true;
	else m_state->info.remove("protected_content_masked");
	ComPtr<ID3D11Texture2D> texture;
	bool croppedOnGpu = false;
	result = resource.As(&texture);
	if (SUCCEEDED(result))
	{
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		m_state->info["source_format"] = static_cast<int>(desc.Format);
		m_state->info["gpu_color_conversion"] = false;
		m_state->info["gpu_resize"] = false;
		if (desc.Format != m_state->nativeFormat || m_state->whiteRefresh.elapsed() > 1000)
		{
			m_state->nativeFormat = desc.Format;
			DXGI_OUTPUT_DESC1 color{};
			if (m_state->outputMetadata && SUCCEEDED(m_state->outputMetadata->GetDesc1(&color)))
				m_state->hdr = color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
			m_state->white = ScreenSdrWhiteNits(m_state->deviceName, &m_state->whiteKnown);
			m_state->whiteRefresh.restart();
			m_state->info["hdr_active"] = m_state->hdr;
			m_state->info["system_sdr_white_nits"] = m_state->white;
			m_state->info["white_detected"] = m_state->whiteKnown;
		}

		bool fp16 = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
		bool rgb10 = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
		bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
		bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
		if (!fp16 && !rgb10 && !bgra && !rgba)
			result = E_NOTIMPL;
		else if (!bgra || std::abs(config.screenExposureEV) > .0001)
		{

			double white = config.screenWhiteNits > 0 ? config.screenWhiteNits : m_state->white;
			int encoding = fp16 ? 1 : rgb10 && m_state->hdr ? 2 : 0;
			QString error;
			const auto local = area.translated(-m_state->bounds.topLeft());
			croppedOnGpu = m_state->rotation == DXGI_MODE_ROTATION_IDENTITY &&
				QRect(0, 0, static_cast<int>(desc.Width), static_cast<int>(desc.Height)).contains(local);
			KRRStageTimer submit(timings, KRRRecordingTimings::ColorSubmit);
			auto converted = m_state->color.convert(m_state->device.Get(), m_state->context.Get(), texture.Get(),
													encoding, white, config.screenExposureEV,
													encoding != 0 && config.screenColor == "highlights", error,
													croppedOnGpu ? local : QRect{}, croppedOnGpu ? outputSize : QSize{},
													config.outputColor == "hdr" && m_state->hdr && encoding != 0);
			submit.finish();
			m_state->info["sdr_white_nits"] = white;
			m_state->info["exposure_ev"] = config.screenExposureEV;
			m_state->info["color_mode"] = config.screenColor;
			m_state->info["gpu_color_conversion"] = true;
			m_state->info["gpu_resize"] = croppedOnGpu && local.size() != outputSize;
			m_state->info["warning"] = error;
			if (converted)
				texture = converted;
			else
				result = E_FAIL;
		}
		if (SUCCEEDED(result))
		{
			texture->GetDesc(&desc);
			D3D11_TEXTURE2D_DESC old{};
			if (m_state->staging)
				m_state->staging->GetDesc(&old);
			if (!m_state->staging || desc.Width != old.Width || desc.Height != old.Height || desc.Format != old.Format)
			{
				m_state->staging.Reset();
				desc.Usage = D3D11_USAGE_STAGING;
				desc.BindFlags = 0;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				desc.MiscFlags = 0;
				result = m_state->device->CreateTexture2D(&desc, nullptr, &m_state->staging);
			}
		}
	}
	if (SUCCEEDED(result))
	{
		KRRStageTimer readback(timings, KRRRecordingTimings::Readback);
		m_state->context->CopyResource(m_state->staging.Get(), texture.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		result = m_state->context->Map(m_state->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
		readback.finish();
		if (SUCCEEDED(result))
		{
			KRRStageTimer pixels(timings, KRRRecordingTimings::DesktopPixels);
			D3D11_TEXTURE2D_DESC description{};
			texture->GetDesc(&description);
			QImage image(static_cast<uchar *>(mapped.pData), static_cast<int>(description.Width),
						 static_cast<int>(description.Height), static_cast<int>(mapped.RowPitch),
						 description.Format == DXGI_FORMAT_R16G16B16A16_UNORM ? QImage::Format_RGBA64 : QImage::Format_RGB32);
			m_state->info["hdr_preserved"] = image.format() == QImage::Format_RGBA64;
			if (m_state->rotation == DXGI_MODE_ROTATION_ROTATE90)
				image = image.transformed(QTransform().rotate(90));
			else if (m_state->rotation == DXGI_MODE_ROTATION_ROTATE180)
				image = image.transformed(QTransform().rotate(180));
			else if (m_state->rotation == DXGI_MODE_ROTATION_ROTATE270)
				image = image.transformed(QTransform().rotate(270));
			if (croppedOnGpu)
				m_state->cached = image.copy();
			else
			{
				const auto local = area.translated(-m_state->bounds.topLeft());
				m_state->cached = local == image.rect() && image.size() != outputSize ?
					image.scaled(outputSize, Qt::IgnoreAspectRatio, Qt::FastTransformation) :
					image.copy(local).scaled(outputSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
			}
			m_state->failure.invalidate();
			m_state->info["reconnecting"] = false;
			m_state->context->Unmap(m_state->staging.Get(), 0);
		}
	}
	m_state->duplication->ReleaseFrame();
	return SUCCEEDED(result) ? m_state->cached : QImage{};
}

QJsonObject KRRDesktopCapture::diagnostics() const
{
	return m_state->info;
}
