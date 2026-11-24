#ifndef WPSRECORDER_DESKTOPCOLOR_H
#define WPSRECORDER_DESKTOPCOLOR_H
#include <QString>
#include <QRect>
#include <QSize>
#include <memory>
#include <QJsonObject>
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
double ScreenSdrWhiteNits(const QString &device, bool *known = nullptr);
QJsonObject ScreenColorMetadata(const QString &device);
// Convert the actual texture format on the GPU, before interpreting any pixels as BGRA8.
class KRRDesktopColor
{
  public:
	KRRDesktopColor();
	~KRRDesktopColor();
	ID3D11Texture2D *convert(ID3D11Device *, ID3D11DeviceContext *, ID3D11Texture2D *, int encoding, double whiteNits,
							 double exposureEV, bool compress, QString &error, const QRect& crop = {}, const QSize& output = {}, bool hdrOutput = false);

  private:
	struct State;
	std::unique_ptr<State> d;
};
#endif
