#include "framecapture.h"
#include "displaygeometry.h"
#include <QGuiApplication>
#include <QJsonDocument>
#include <QFile>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
int main(int argc, char **argv)
{
	InitializeDisplayCoordinates();
	QGuiApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QGuiApplication app(argc, argv);
	QJsonArray results;
	ComPtr<IDXGIFactory1> factory; CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	for (UINT a=0;factory;++a) {
		ComPtr<IDXGIAdapter1> adapter; if(factory->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND)break;
		for(UINT o=0;;++o) {
			ComPtr<IDXGIOutput> output;if(adapter->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND)break;
			DXGI_OUTPUT_DESC desc{};output->GetDesc(&desc);if(!desc.AttachedToDesktop)continue;
			QJsonObject item{{"device",QString::fromWCharArray(desc.DeviceName)}};
			ComPtr<IDXGIOutput6> out6;DXGI_OUTPUT_DESC1 desc1{};
			if(SUCCEEDED(output.As(&out6))&&SUCCEEDED(out6->GetDesc1(&desc1))){item["color_space"]=static_cast<int>(desc1.ColorSpace);item["bits"]=static_cast<int>(desc1.BitsPerColor);item["max_nits"]=desc1.MaxLuminance;}
			ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;
			HRESULT hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
			ComPtr<IDXGIOutput1> output1;ComPtr<IDXGIOutputDuplication> duplication;
			if(SUCCEEDED(hr))hr=output.As(&output1);
			if(SUCCEEDED(hr))hr=output1->DuplicateOutput(device.Get(),&duplication);
			if(SUCCEEDED(hr)) {
				DXGI_OUTDUPL_DESC dupDesc{};duplication->GetDesc(&dupDesc);item["duplication_format"]=static_cast<int>(dupDesc.ModeDesc.Format);
				DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> resource;hr=duplication->AcquireNextFrame(1000,&info,&resource);
				if(SUCCEEDED(hr)){ComPtr<ID3D11Texture2D> texture;resource.As(&texture);D3D11_TEXTURE2D_DESC t{};texture->GetDesc(&t);item["frame_format"]=static_cast<int>(t.Format);item["width"]=static_cast<int>(t.Width);item["height"]=static_cast<int>(t.Height);duplication->ReleaseFrame();}
			}
			item["hr"]=QString::number(static_cast<quint32>(hr),16);results.append(item);
		}
	}
	QJsonObject report{{"monitors",KRRFrameCapture::Monitors()},{"dxgi",results}};
	QFile f(argc>1?argv[1]:"display-probe.json");if(!f.open(QIODevice::WriteOnly))return 1;f.write(QJsonDocument(report).toJson());
	return 0;
}
