#include "windowidentity.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

static HRESULT SetText(IPropertyStore* store, const PROPERTYKEY& key, const std::wstring& text)
{
	PROPVARIANT value;
	HRESULT result = InitPropVariantFromString(text.c_str(), &value);
	if (FAILED(result)) return result;
	result = store->SetValue(key, value);
	PropVariantClear(&value);
	return result;
}

KRRWindowIdentity::KRRWindowIdentity(HWND window) : m_window(window)
{
	ComPtr<IPropertyStore> properties;
	if (FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&properties)))) return;
	const auto exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
	const auto command = L"\"" + exe + L"\"";
	if (FAILED(SetText(properties.Get(), PKEY_AppUserModel_RelaunchCommand, command)) ||
		FAILED(SetText(properties.Get(), PKEY_AppUserModel_RelaunchDisplayNameResource, L"@" + exe + L",-102")) ||
		FAILED(SetText(properties.Get(), PKEY_AppUserModel_RelaunchIconResource, exe + L",-101")) ||
		FAILED(SetText(properties.Get(), PKEY_AppUserModel_ID, RecorderAppId)))
		qWarning("WPS Recorder could not set its taskbar window properties.");
}

KRRWindowIdentity::~KRRWindowIdentity()
{
	ComPtr<IPropertyStore> properties;
	if (FAILED(SHGetPropertyStoreForWindow(m_window, IID_PPV_ARGS(&properties)))) return;
	PROPVARIANT empty{};
	properties->SetValue(PKEY_AppUserModel_ID, empty);
	properties->SetValue(PKEY_AppUserModel_RelaunchIconResource, empty);
	properties->SetValue(PKEY_AppUserModel_RelaunchDisplayNameResource, empty);
	properties->SetValue(PKEY_AppUserModel_RelaunchCommand, empty);
}

bool CreateRecorderShortcut(const QString& output, QString& error)
{
	const QFileInfo path(output);
	const QDir install(QCoreApplication::applicationDirPath());
	const auto executable = install.filePath("wpsrecorder.exe");
	if (!path.isAbsolute() || path.suffix().compare("lnk", Qt::CaseInsensitive) != 0 ||
		!path.dir().exists() || !QFileInfo(executable).isFile())
	{
		error = "Use an absolute .lnk output path in an existing folder and run from the installed package.";
		return false;
	}
	const auto exe = QDir::toNativeSeparators(executable).toStdWString();
	const auto folder = QDir::toNativeSeparators(install.path()).toStdWString();
	const auto shortcut = QDir::toNativeSeparators(path.absoluteFilePath()).toStdWString();
	ComPtr<IShellLinkW> link;
	ComPtr<IPropertyStore> properties;
	ComPtr<IPersistFile> file;
	const bool existed = path.exists();
	HRESULT result;
	if (FAILED(result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
		FAILED(result = link->SetPath(exe.c_str())) ||
		FAILED(result = link->SetWorkingDirectory(folder.c_str())) ||
		FAILED(result = link->SetIconLocation(exe.c_str(), -101)) ||
		FAILED(result = link.As(&properties)) ||
		FAILED(result = SetText(properties.Get(), PKEY_AppUserModel_ID, RecorderAppId)) ||
		FAILED(result = properties->Commit()) ||
		FAILED(result = link.As(&file)) ||
		FAILED(result = file->Save(shortcut.c_str(), TRUE)))
	{
		error = QStringLiteral("Could not save the WPS Recorder shortcut (HRESULT 0x%1).")
			.arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
		return false;
	}
	SHChangeNotify(existed ? SHCNE_UPDATEITEM : SHCNE_CREATE, SHCNF_PATHW, shortcut.c_str(), nullptr);
	return true;
}
