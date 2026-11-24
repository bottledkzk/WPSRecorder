#include "desktopcolor.h"
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
double ScreenSdrWhiteNits(const QString &device, bool *known)
{
	if (known)
		*known = false;
	for (int retry = 0; retry < 3; ++retry)
	{
		UINT32 paths = 0, modes = 0;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS)
			break;
		std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
		std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
		LONG result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr);
		if (result == ERROR_INSUFFICIENT_BUFFER)
			continue;
		if (result != ERROR_SUCCESS)
			break;
		for (UINT32 i = 0; i < paths; ++i)
		{
			DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
			source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), p[i].sourceInfo.adapterId,
							 p[i].sourceInfo.id};
			if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
				device != QString::fromWCharArray(source.viewGdiDeviceName))
				continue;
			DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
			white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white), p[i].targetInfo.adapterId,
							p[i].targetInfo.id};
			if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
			{
				if (known)
					*known = true;
				return white.SDRWhiteLevel * .08;
			}
		}
		break;
	}
	return 80.;
}
QJsonObject ScreenColorMetadata(const QString &name)
{
	bool known = false;
	double white = ScreenSdrWhiteNits(name, &known);
	QJsonObject info{{"system_sdr_white_nits", white}, {"white_detected", known}, {"hdr_active", false}};
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return info;
	for (UINT a = 0;; ++a)
	{
		ComPtr<IDXGIAdapter1> adapter;
		if (FAILED(factory->EnumAdapters1(a, &adapter)))
			break;
		for (UINT o = 0;; ++o)
		{
			ComPtr<IDXGIOutput> output;
			if (FAILED(adapter->EnumOutputs(o, &output)))
				break;
			DXGI_OUTPUT_DESC d{};
			if (FAILED(output->GetDesc(&d)) || name != QString::fromWCharArray(d.DeviceName))
				continue;
			ComPtr<IDXGIOutput6> six;
			DXGI_OUTPUT_DESC1 desc{};
			if (SUCCEEDED(output.As(&six)) && SUCCEEDED(six->GetDesc1(&desc)))
			{
				info["hdr_active"] = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
				info["output_bits_per_color"] = static_cast<int>(desc.BitsPerColor);
				info["output_color_space"] = static_cast<int>(desc.ColorSpace);
			}
			return info;
		}
	}
	return info;
}
static const char Shader[] = R"(
Texture2D<float4> source : register(t0);
cbuffer Params : register(b0) { float encoding; float scale; float exposure; float compress; float2 offset; float2 sampleScale; float hdrOutput; };
float4 vs(uint id : SV_VertexID) : SV_POSITION {
 float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
float3 decodeSrgb(float3 x) { return float3(x.r<=.04045?x.r/12.92:pow((x.r+.055)/1.055,2.4), x.g<=.04045?x.g/12.92:pow((x.g+.055)/1.055,2.4), x.b<=.04045?x.b/12.92:pow((x.b+.055)/1.055,2.4)); }
float3 encodeSrgb(float3 x) { return float3(x.r<=.0031308?12.92*x.r:1.055*pow(x.r,1./2.4)-.055, x.g<=.0031308?12.92*x.g:1.055*pow(x.g,1./2.4)-.055, x.b<=.0031308?12.92*x.b:1.055*pow(x.b,1./2.4)-.055); }
float3 encodePq(float3 nits) {
 float3 p=pow(saturate(nits/10000.),.1593017578125);
 return pow((.8359375+18.8515625*p)/(1.+18.6875*p),78.84375);
}
float4 ps(float4 position : SV_POSITION) : SV_TARGET {
 float3 c=source.Load(int3(int2(position.xy*sampleScale+offset),0)).rgb;
 if(hdrOutput>.5) {
  if(encoding>1.5) return float4(saturate(c),1);
  c=mul(float3x3(.627404,.329282,.0433136,.069097,.91954,.0113612,.0163916,.0880132,.895595),c);
  return float4(encodePq(max(c*80.,0)),1);
 }
 if(encoding<.5)c=decodeSrgb(saturate(c));
 else if(encoding>1.5) {
  float3 p=pow(saturate(c),1./78.84375);
  c=pow(max(p-.8359375,0)/max(18.8515625-18.6875*p,1e-6),1./.1593017578125)*125.;
  c=mul(float3x3(1.660491,-.587641,-.07285,-.12455,1.1329,-.008349,-.018151,-.100579,1.11873),c);
 }
 c=max(c,0)*scale*exposure;
 if(compress>.5) { float peak=max(c.r,max(c.g,c.b)); if(peak>.75) {float t=peak-.75;float mapped=.75+.25*t/(t+.25);c*=mapped/peak;} }
 return float4(encodeSrgb(saturate(c)),1);
}
)";
struct KRRDesktopColor::State
{
	ID3D11Device *device = nullptr;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> ps;
	ComPtr<ID3D11Buffer> params;
	ComPtr<ID3D11Texture2D> source, target;
	ComPtr<ID3D11ShaderResourceView> srv;
	ComPtr<ID3D11RenderTargetView> rtv;
	D3D11_TEXTURE2D_DESC description{};
	QSize targetSize;
	bool hdrOutput = false;
};
KRRDesktopColor::KRRDesktopColor() : d(std::make_unique<State>())
{
}
KRRDesktopColor::~KRRDesktopColor() = default;
ID3D11Texture2D *KRRDesktopColor::convert(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *input,
										  int encoding, double whiteNits, double exposureEV, bool compress,
										  QString &error, const QRect& crop, const QSize& output, bool hdrOutput)
{
	if (d->device != device)
	{
		d = std::make_unique<State>();
		d->device = device;
	}
	HRESULT hr = S_OK;
	if (!d->vs)
	{
		ComPtr<ID3DBlob> vs, ps, errors;
		hr = D3DCompile(Shader, strlen(Shader), nullptr, nullptr, nullptr, "vs", "vs_4_0",
						D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors);
		if (SUCCEEDED(hr))
			hr = D3DCompile(Shader, strlen(Shader), nullptr, nullptr, nullptr, "ps", "ps_4_0",
							D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors);
		if (SUCCEEDED(hr))
			hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &d->vs);
		if (SUCCEEDED(hr))
			hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &d->ps);
		D3D11_BUFFER_DESC b{};
		b.ByteWidth = 48;
		b.Usage = D3D11_USAGE_DEFAULT;
		b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (SUCCEEDED(hr))
			hr = device->CreateBuffer(&b, nullptr, &d->params);
		if (FAILED(hr))
		{
			error = "HDR shader initialization failed: " + QString::number(static_cast<quint32>(hr), 16);
			d = std::make_unique<State>();
			return nullptr;
		}
	}
	D3D11_TEXTURE2D_DESC desc{};
	input->GetDesc(&desc);
	const QRect sourceRect = crop.isEmpty() ? QRect(0, 0, static_cast<int>(desc.Width), static_cast<int>(desc.Height)) : crop;
	const QSize targetSize = output.isEmpty() ? sourceRect.size() : output;
	if (!d->target || desc.Width != d->description.Width || desc.Height != d->description.Height ||
		desc.Format != d->description.Format || targetSize != d->targetSize || hdrOutput != d->hdrOutput)
	{
		d->description = desc;
		d->targetSize = targetSize;
		d->hdrOutput = hdrOutput;
		d->srv.Reset();
		d->rtv.Reset();
		d->source.Reset();
		d->target.Reset();
		auto s = desc;
		s.Usage = D3D11_USAGE_DEFAULT;
		s.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		s.CPUAccessFlags = 0;
		s.MiscFlags = 0;
		hr = device->CreateTexture2D(&s, nullptr, &d->source);
		if (SUCCEEDED(hr))
			hr = device->CreateShaderResourceView(d->source.Get(), nullptr, &d->srv);
		s.Width = static_cast<UINT>(targetSize.width());
		s.Height = static_cast<UINT>(targetSize.height());
		s.Format = hdrOutput ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
		s.BindFlags = D3D11_BIND_RENDER_TARGET;
		if (SUCCEEDED(hr))
			hr = device->CreateTexture2D(&s, nullptr, &d->target);
		if (SUCCEEDED(hr))
			hr = device->CreateRenderTargetView(d->target.Get(), nullptr, &d->rtv);
		if (FAILED(hr))
		{
			error = "HDR texture conversion unavailable: " + QString::number(static_cast<quint32>(hr), 16);
			d->target.Reset();
			return nullptr;
		}
	}
	float p[12] = {static_cast<float>(encoding), encoding == 0 ? 1.f : static_cast<float>(80. / qMax(1., whiteNits)),
				  static_cast<float>(std::exp2(exposureEV)), compress ? 1.f : 0.f,
				  static_cast<float>(sourceRect.x()), static_cast<float>(sourceRect.y()),
				  static_cast<float>(sourceRect.width()) / targetSize.width(),
				  static_cast<float>(sourceRect.height()) / targetSize.height(), hdrOutput ? 1.f : 0.f, 0.f, 0.f, 0.f};
	context->CopyResource(d->source.Get(), input);
	context->UpdateSubresource(d->params.Get(), 0, nullptr, p, 0, 0);
	D3D11_VIEWPORT viewport{0, 0, static_cast<float>(targetSize.width()), static_cast<float>(targetSize.height()), 0, 1};
	context->RSSetViewports(1, &viewport);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(d->vs.Get(), nullptr, 0);
	context->PSSetShader(d->ps.Get(), nullptr, 0);
	ID3D11Buffer *buffer = d->params.Get();
	context->PSSetConstantBuffers(0, 1, &buffer);
	ID3D11ShaderResourceView *srv = d->srv.Get();
	context->PSSetShaderResources(0, 1, &srv);
	ID3D11RenderTargetView *rtv = d->rtv.Get();
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->Draw(3, 0);
	srv = nullptr;
	context->PSSetShaderResources(0, 1, &srv);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	return d->target.Get();
}
