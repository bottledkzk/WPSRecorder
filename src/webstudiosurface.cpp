#include "webstudiosurface.h"

#include "recordingconfig.h"
#include "uilanguage.h"

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
#include <QUuid>
#include <QUrl>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace
{
const QString Host = "studio.wpsrecorder.invalid";
const QString Home = "https://studio.wpsrecorder.invalid/index.html";
const QByteArray StudioOrigin = "https://studio.wpsrecorder.invalid";
QString ReadString(HRESULT hr, PWSTR value)
{
	const QString text = SUCCEEDED(hr) && value ? QString::fromWCharArray(value) : QString{};
	CoTaskMemFree(value);
	return text;
}
struct MediaTransfer
{
	QFile file;
	QByteArray request;
	qint64 left = 0;
	bool started = false;
};

void ContinueTransfer(QTcpSocket* socket, MediaTransfer& transfer)
{
	while (transfer.left > 0 && socket->bytesToWrite() < 256 * 1024)
	{
		const auto bytes = transfer.file.read(qMin<qint64>(transfer.left, 64 * 1024));
		if (bytes.isEmpty() || socket->write(bytes) < 0) { socket->abort(); return; }
		transfer.left -= bytes.size();
	}
	if (!transfer.left)
	{
		transfer.file.close();
		socket->disconnectFromHost();
	}
}
}

struct KRRWebStudioSurface::Private
{
	ComPtr<ICoreWebView2Environment> environment;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2> browser;
	QHash<QString, QString> files, ids;
	QTcpServer media;
	QByteArray token = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
	bool coInitialized = false;
};

KRRWebStudioSurface::KRRWebStudioSurface(QWidget* parent)
	: QWidget(parent), p(std::make_unique<Private>())
{
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setObjectName("studioWebSurface");
	setStyleSheet("background:#171b20");
}

KRRWebStudioSurface::~KRRWebStudioSurface()
{
	if (p->controller) p->controller->Close();
	p->browser.Reset(); p->controller.Reset(); p->environment.Reset();
	if (p->coInitialized) CoUninitialize();
}

void KRRWebStudioSurface::start()
{
	if (!p->media.listen(QHostAddress::LocalHost))
	{
		emit failed(Ui("无法启动本地预览服务。"));
		return;
	}
	connect(&p->media, &QTcpServer::newConnection, this, [this]
	{
		while (auto socket = p->media.nextPendingConnection())
		{
			auto state = std::make_shared<MediaTransfer>();
			connect(socket, &QTcpSocket::disconnected, socket, [socket, state]
			{
				state->file.close();
				socket->deleteLater();
			});
			connect(socket, &QTcpSocket::bytesWritten, socket, [socket, state]
			{
				if (state->started) ContinueTransfer(socket, *state);
			});
			connect(socket, &QTcpSocket::readyRead, socket, [this, socket, state]
			{
				if (state->started) return;
				state->request += socket->readAll();
				if (state->request.size() > 16 * 1024) { socket->abort(); return; }
				const int headerEnd = state->request.indexOf("\r\n\r\n");
				if (headerEnd < 0) return;
				const auto rows = state->request.left(headerEnd).split('\n');
				const auto line = rows.value(0).simplified().split(' ');
				const auto method = line.value(0);
				const auto prefix = "/media/" + p->token + "/";
				const auto path = line.value(1);
				const auto id = path.startsWith(prefix) ? path.mid(prefix.size()) : QByteArray{};
				const auto file = fileForResource(QString::fromLatin1(id));
				QByteArray range, origin;
				for (const auto& row : rows)
				{
					const int colon = row.indexOf(':');
					if (colon < 0) continue;
					const auto key = row.left(colon).toLower();
					if (key == "range") range = row.mid(colon + 1).trimmed();
					if (key == "origin") origin = row.mid(colon + 1).trimmed();
				}
				const auto finish = [socket](const QByteArray& header)
				{
					socket->write(header);
					socket->disconnectFromHost();
				};
				const auto allowed = "Access-Control-Allow-Origin: " + StudioOrigin +
					"\r\nAccess-Control-Allow-Private-Network: true\r\nCross-Origin-Resource-Policy: cross-origin\r\n"
					"Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\nAccess-Control-Allow-Headers: Range\r\n"
					"Accept-Ranges: bytes\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n";
				if (file.isEmpty() || (!origin.isEmpty() && origin != StudioOrigin))
				{
					finish("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
					return;
				}
				if (method == "OPTIONS")
				{
					finish("HTTP/1.1 204 No Content\r\n" + allowed + "\r\n");
					return;
				}
				state->file.setFileName(file);
				if ((method != "GET" && method != "HEAD") || !state->file.open(QIODevice::ReadOnly))
				{
					finish("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n" + allowed + "\r\n");
					return;
				}
				const auto length = state->file.size();
				socket->setProperty("media-id", QString::fromLatin1(id));
				qint64 first = 0, last = length - 1;
				if (!range.isEmpty())
				{
					static const QRegularExpression pattern("^bytes=(\\d{0,18})-(\\d{0,18})$");
					const auto match = pattern.match(QString::fromLatin1(range));
					const auto begin = match.captured(1), end = match.captured(2);
					if (!match.hasMatch() || (begin.isEmpty() && (end.isEmpty() || end.toLongLong() == 0))) first = length;
					else if (begin.isEmpty()) first = qMax<qint64>(0, length - end.toLongLong());
					else
					{
						first = begin.toLongLong();
						if (!end.isEmpty()) last = qMin(last, end.toLongLong());
					}
				}
				if (first > last || !state->file.seek(first))
				{
					state->file.close();
					finish("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nContent-Range: bytes */" +
						QByteArray::number(length) + "\r\n" + allowed + "\r\n");
					return;
				}
				state->left = last - first + 1;
				const auto mime = QMimeDatabase().mimeTypeForFile(file, QMimeDatabase::MatchExtension).name().toLatin1();
				QByteArray header = range.isEmpty() ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 206 Partial Content\r\n";
				header += "Content-Type: " + mime + "\r\nContent-Length: " + QByteArray::number(state->left) + "\r\n";
				if (!range.isEmpty())
					header += "Content-Range: bytes " + QByteArray::number(first) + "-" + QByteArray::number(last) + "/" + QByteArray::number(length) + "\r\n";
				header += allowed + "\r\n";
				if (method == "HEAD") { state->file.close(); finish(header); return; }
				state->started = true;
				socket->write(header);
				ContinueTransfer(socket, *state);
			});
		}
	});
	const auto bundle = QCoreApplication::applicationDirPath() + "/studio";
	if (!QFileInfo::exists(bundle + "/index.html"))
	{
		emit failed(Ui("缺少 Studio 界面文件，请重新解压完整程序。"));
		return;
	}
	p->coInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
	const auto userData = QDir::toNativeSeparators(DataDirectory() + "/WebView2");
	const QPointer<KRRWebStudioSurface> self(this);
	const auto environment = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
		[self, bundle](HRESULT status, ICoreWebView2Environment* value) -> HRESULT
	{
		if (!self || !value || FAILED(status))
		{
			if (self) emit self->failed(Ui("无法启动 Microsoft Edge WebView2 Runtime。"));
			return S_OK;
		}
		self->p->environment = value;
		const auto controller = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
			[self, bundle](HRESULT created, ICoreWebView2Controller* control) -> HRESULT
		{
			if (!self) return S_OK;
			if (!control || FAILED(created)) { emit self->failed(Ui("无法创建 Studio 界面。")); return S_OK; }
			self->p->controller = control;
			control->get_CoreWebView2(&self->p->browser);
			auto core = self->p->browser.Get();
			ComPtr<ICoreWebView2_3> mapped;
			if (!core || FAILED(self->p->browser.As(&mapped)))
			{
				emit self->failed(Ui("请更新 Microsoft Edge WebView2 Runtime。"));
				return S_OK;
			}
			ComPtr<ICoreWebView2Settings> settings;
			core->get_Settings(&settings);
			settings->put_AreDefaultContextMenusEnabled(FALSE);
			settings->put_AreDevToolsEnabled(FALSE);
			settings->put_IsStatusBarEnabled(FALSE);
			ComPtr<ICoreWebView2Settings4> formSettings;
			if (SUCCEEDED(settings.As(&formSettings)))
			{
				formSettings->put_IsPasswordAutosaveEnabled(FALSE);
				formSettings->put_IsGeneralAutofillEnabled(FALSE);
			}
			const auto site = Host.toStdWString();
			const auto directory = QDir::toNativeSeparators(bundle).toStdWString();
			mapped->SetVirtualHostNameToFolderMapping(site.c_str(), directory.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
			EventRegistrationToken unused{};
			core->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
				[](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT
			{
				PWSTR uri = nullptr;
				const auto result = args->get_Uri(&uri);
				if (ReadString(result, uri) != Home) args->put_Cancel(TRUE);
				return S_OK;
			}).Get(), &unused);
			core->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
				[](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { return args->put_Handled(TRUE); }
			).Get(), &unused);
			core->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
				[self](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
			{
				if (!self) return S_OK;
				PWSTR source = nullptr;
				const auto origin = args->get_Source(&source);
				if (ReadString(origin, source) != Home) return S_OK;
				PWSTR json = nullptr;
				const auto status = args->get_WebMessageAsJson(&json);
				const auto text = ReadString(status, json);
				if (text.size() > 2 * 1024 * 1024) return S_OK;
				const auto doc = QJsonDocument::fromJson(text.toUtf8());
				if (doc.isObject())
					QMetaObject::invokeMethod(self, [self, message = doc.object()] { if (self) emit self->request(message); }, Qt::QueuedConnection);
				return S_OK;
			}).Get(), &unused);
			RECT bounds{0, 0, self->width(), self->height()};
			control->put_Bounds(bounds);
			control->put_IsVisible(TRUE);
			core->Navigate(Home.toStdWString().c_str());
			emit self->ready();
			return S_OK;
		});
		const auto result = value->CreateCoreWebView2Controller(reinterpret_cast<HWND>(self->winId()), controller.Get());
		if (FAILED(result)) emit self->failed(Ui("无法创建 Studio 界面。"));
		return S_OK;
	});
	const auto result = CreateCoreWebView2EnvironmentWithOptions(nullptr,
		userData.toStdWString().c_str(), nullptr, environment.Get());
	if (FAILED(result)) emit failed(Ui("无法启动 Microsoft Edge WebView2 Runtime。"));
}

bool KRRWebStudioSurface::send(const QJsonObject& message)
{
	if (!p->browser) return false;
	const auto json = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
	return SUCCEEDED(p->browser->PostWebMessageAsJson(json.toStdWString().c_str()));
}

QString KRRWebStudioSurface::localResource(const QString& file)
{
	const QFileInfo info(file);
	if (!info.isFile()) return {};
	const auto path = info.canonicalFilePath();
	if (path.isEmpty()) return {};
	const auto ext = info.suffix().toLower();
	if (!QStringList{"mp4", "mov", "mkv", "webm", "avi", "flv", "m4v", "mp3", "m4a", "aac", "ogg", "wav",
		"flac", "opus", "png", "jpg", "jpeg", "bmp", "webp"}.contains(ext)) return {};
	auto id = p->ids.value(path);
	if (id.isEmpty())
	{
		id = QString::fromLatin1(QCryptographicHash::hash(path.toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex().left(32)) + "." + ext;
		p->ids[path] = id;
		p->files[id] = path;
	}
	return "http://127.0.0.1:" + QString::number(p->media.serverPort()) + "/media/" + QString::fromLatin1(p->token) + "/" + id;
}

QString KRRWebStudioSurface::fileForResource(const QString& id) const
{
	return p->files.value(id);
}

void KRRWebStudioSurface::releaseResource(const QString& id)
{
	const auto path = p->files.take(id);
	if (path.isEmpty()) return;
	p->ids.remove(path);
	for (auto socket : p->media.findChildren<QTcpSocket*>(QString{}, Qt::FindDirectChildrenOnly))
		if (socket->property("media-id").toString() == id) socket->abort();
}

void KRRWebStudioSurface::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
	if (p->controller)
		p->controller->put_Bounds(RECT{0, 0, width(), height()});
}
