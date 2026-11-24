#include "desktopcolor.h"
#include "displaygeometry.h"
#include "framecapture.h"
#include "recordingengine.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <cmath>
#include <d3d11.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
static const QColor Colors[] = {Qt::black, Qt::white, QColor(128, 128, 128), Qt::red,
								Qt::green, Qt::blue,  QColor(64, 64, 64),	 QColor(192, 192, 192)};
class Surface : public QWidget
{
  public:
	int tick = 0;
	Surface()
	{
		setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
	}
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		for (int i = 0; i < 8; ++i)
			p.fillRect(QRect(i * width() / 8, 0, (i + 1) * width() / 8 - i * width() / 8, height()), Colors[i]);
		p.fillRect(QRect(0, 0, width(), 80), QColor("#25374f"));
		p.setPen(Qt::white);
		p.drawText(30, 45, QString("WPS Recorder · HDR/SDR native color acceptance · %1").arg(tick));
	}
};
static bool SampleMatches(const QImage &image, int tolerance, QJsonArray &pixels)
{
	if (image.isNull())
		return false;
	bool valid = true;
	for (int i = 0; i < 8; ++i)
	{
		auto c = image.pixelColor((2 * i + 1) * image.width() / 16, image.height() / 2);
		auto e = Colors[i];
		int delta = qMax(qAbs(c.red() - e.red()), qMax(qAbs(c.green() - e.green()), qAbs(c.blue() - e.blue())));
		valid &= delta <= tolerance;
		pixels.append(QJsonObject{{"expected", e.name()}, {"actual", c.name()}, {"max_error", delta}});
	}
	return valid;
}
static QImage ReadTexture(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *texture)
{
	if (!texture)
		return {};
	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	ComPtr<ID3D11Texture2D> copy;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &copy)))
		return {};
	ctx->CopyResource(copy.Get(), texture);
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(ctx->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
		return {};
	QImage image(static_cast<uchar *>(mapped.pData), desc.Width, desc.Height, mapped.RowPitch, QImage::Format_RGB32);
	auto result = image.copy();
	ctx->Unmap(copy.Get(), 0);
	return result;
}
static bool Synthetic(QJsonArray &results)
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	D3D_FEATURE_LEVEL level;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
								 &level, &context)))
		return false;
	KRRDesktopColor converter;
	QString error;
	// RGBA half-floats: scRGB 2.0 = 160nit white, 1.0 = half that, channels tested independently.
	const unsigned short pixels[] = {
		0, 0,	   0, 0x3c00, 0x4000, 0x4000, 0x4000, 0x3c00, 0x3c00, 0x3c00, 0x3c00, 0x3c00, 0x4000, 0, 0, 0x3c00,
		0, 0x4000, 0, 0x3c00, 0,	  0,	  0x4000, 0x3c00, 0x4400, 0x4400, 0x4400, 0x3c00, 0xbc00, 0, 0, 0x3c00};
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = 8;
	desc.Height = 1;
	desc.MipLevels = desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	D3D11_SUBRESOURCE_DATA data{pixels, sizeof(pixels), 0};
	ComPtr<ID3D11Texture2D> input;
	if (FAILED(device->CreateTexture2D(&desc, &data, &input)))
		return false;
	auto image = ReadTexture(device.Get(), context.Get(),
							 converter.convert(device.Get(), context.Get(), input.Get(), 1, 160, 0, false, error));
	if (image.isNull())
	{
		results.append(QJsonObject{{"error", error}});
		return false;
	}
	QColor fpExpected[] = {Qt::black, Qt::white, QColor(188, 188, 188), Qt::red, Qt::green, Qt::blue,
						   Qt::white, Qt::black};
	bool ok = true;
	for (int i = 0; i < 8; ++i)
	{
		auto c = image.pixelColor(i, 0);
		ok &= qAbs(c.red() - fpExpected[i].red()) <= 1 && qAbs(c.green() - fpExpected[i].green()) <= 1 &&
			  qAbs(c.blue() - fpExpected[i].blue()) <= 1;
	}
	results.append(QJsonObject{{"case", "fp16_gpu_formats_and_white"}, {"passed", ok}});
	auto dark = ReadTexture(device.Get(), context.Get(),
							converter.convert(device.Get(), context.Get(), input.Get(), 1, 160, -1, false, error));
	bool exposure = !dark.isNull() && qAbs(dark.pixelColor(1, 0).red() - 188) <= 1;
	results.append(QJsonObject{{"case", "gpu_exposure_minus_one"}, {"passed", exposure}});
	auto roll = ReadTexture(device.Get(), context.Get(),
							converter.convert(device.Get(), context.Get(), input.Get(), 1, 160, 0, true, error));
	bool shoulder = !roll.isNull() && roll.pixelColor(6, 0).red() > roll.pixelColor(1, 0).red() &&
					roll.pixelColor(6, 0).red() < 255;
	results.append(QJsonObject{{"case", "highlight_rolloff"}, {"passed", shoulder}});
	bool formats = true;
	for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM})
	{
		unsigned int pixel =
			format == DXGI_FORMAT_R10G10B10A2_UNORM ? (512u | (512u << 10) | (512u << 20) | (3u << 30)) : 0xff0033cc;
		desc.Width = 1;
		desc.Format = format;
		D3D11_SUBRESOURCE_DATA sample{&pixel, 4, 0};
		ComPtr<ID3D11Texture2D> texture;
		bool valid = SUCCEEDED(device->CreateTexture2D(&desc, &sample, &texture));
		int encoding = format == DXGI_FORMAT_R10G10B10A2_UNORM ? 2 : 0;
		auto decoded = valid ? ReadTexture(device.Get(), context.Get(),
										   converter.convert(device.Get(), context.Get(), texture.Get(), encoding, 100,
															 0, false, error))
							 : QImage{};
		QColor expected = format == DXGI_FORMAT_R8G8B8A8_UNORM ? QColor(204, 51, 0) : QColor(0, 51, 204);
		if (encoding == 2)
		{
			double code = 512. / 1023.;
			double p = std::pow(code, 1. / 78.84375);
			double linear =
				10000 * std::pow(qMax(p - .8359375, 0.) / (18.8515625 - 18.6875 * p), 1. / .1593017578125) / 100.;
			int v = qRound(255 * (1.055 * std::pow(linear, 1. / 2.4) - .055));
			expected = QColor(v, v, v);
		}
		if (decoded.isNull())
			valid = false;
		else
		{
			auto actual = decoded.pixelColor(0, 0);
			valid &= qAbs(actual.red() - expected.red()) <= 2 && qAbs(actual.green() - expected.green()) <= 2 &&
					 qAbs(actual.blue() - expected.blue()) <= 2;
		}
		formats &= valid;
		results.append(
			QJsonObject{{"case", "native_gpu_format"}, {"format", static_cast<int>(format)}, {"passed", valid}});
	}
	return ok && exposure && shoulder && formats;
}
int main(int argc, char **argv)
{
	InitializeDisplayCoordinates();
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication app(argc, argv);
	app.setOrganizationName("WPSCourse");
	app.setApplicationName("wpsrecorder-color-qa");
	if (argc < 3 || QDir(argv[1]).exists())
		return 2;
	QString root = argv[1], report = argv[2];
	QDir().mkpath(root);
	QJsonArray results;
	bool passed = Synthetic(results);
	Surface surface;
	KRRRecordingEngine engine;
	QTimer animate;
	QElapsedTimer wall;
	int hdrIndex = -1;
	auto monitors = KRRFrameCapture::Monitors();
	for (int i = 0; i < monitors.size(); ++i)
	{
		auto m = monitors[i].toObject();
		surface.setGeometry(m["x"].toInt(), m["y"].toInt(), m["width"].toInt(), m["height"].toInt());
		surface.show();
		surface.raise();
		QElapsedTimer settle;
		settle.start();
		while (settle.elapsed() < 350)
		{
			app.processEvents();
			QThread::msleep(5);
		}
		KRRRecordingConfig config;
		config.monitor = i;
		config.width = m["width"].toInt();
		config.height = m["height"].toInt();
		config.cursor = false;
		KRRFrameCapture capture;
		QString error;
		QImage image;
		for (int n = 0; n < 3; ++n)
		{
			image = capture.capture(config, error);
			app.processEvents();
			QThread::msleep(30);
		}
		QJsonArray pixels;
		bool match = SampleMatches(image, 12, pixels);
		auto diag = capture.diagnostics();
		if (diag["hdr_active"].toBool())
			hdrIndex = i;
		passed &= match && diag["api"] == "DXGI";
		results.append(QJsonObject{{"case", "native_screen_color"},
								   {"monitor", m["friendly_name"]},
								   {"passed", match && diag["api"] == "DXGI"},
								   {"pixels", pixels},
								   {"capture", diag}});
		image.save(root + QString("/screen-%1.png").arg(i));
		surface.hide();
	}
	if (hdrIndex < 0)
	{
		passed = false;
		results.append(QJsonObject{{"error", "No active HDR monitor for this acceptance"}});
	}
	auto write = [&](const QString &error)
	{
		QFile f(report);
		f.open(QIODevice::WriteOnly);
		f.write(QJsonDocument(QJsonObject{{"passed", passed && error.isEmpty()},
										  {"error", error},
										  {"cases", results},
										  {"recording", engine.status()}})
					.toJson());
	};
	if (!passed)
	{
		write("Color reference failed");
		return 1;
	}
	auto m = monitors[hdrIndex].toObject();
	surface.setGeometry(m["x"].toInt(), m["y"].toInt(), m["width"].toInt(), m["height"].toInt());
	surface.show();
	surface.raise();
	app.processEvents();
	KRRRecordingConfig c;
	c.output = root + "/hdr-color-recording.mp4";
	c.width = m["width"].toInt();
	c.height = m["height"].toInt();
	c.monitor = hdrIndex;
	c.cursor = false;
	c.maxDuration = 7;
	c.fps = 30;
	QObject::connect(&animate, &QTimer::timeout,
					 [&]
					 {
						 ++surface.tick;
						 surface.update();
					 });
	animate.start(33);
	QObject::connect(&engine, &KRRRecordingEngine::completed,
					 [&](const QString &, const QString &error)
					 {
						 write(error);
						 app.exit(error.isEmpty() ? 0 : 1);
					 });
	QString error;
	if (!engine.start(c, error))
	{
		write(error);
		return 1;
	}
	QTimer::singleShot(2800,
					   [&]
					   {
						   QString e;
						   if (!engine.updateScene({{"screenExposureEV", -1}}, e))
						   {
							   passed = false;
						   }
					   });
	QTimer::singleShot(5000,
					   [&]
					   {
						   QString e;
						   if (!engine.updateScene({{"screenExposureEV", 0}, {"screenColor", "highlights"}}, e))
						   {
							   passed = false;
						   }
					   });
	QTimer::singleShot(25000,
					   [&]
					   {
						   engine.requestStop();
						   write("Timeout");
						   app.exit(1);
					   });
	return app.exec();
}
