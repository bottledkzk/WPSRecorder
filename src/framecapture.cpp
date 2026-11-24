#include "framecapture.h"
#include "recordingtiming.h"
#include "desktopcolor.h"
#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QPainter>
#include <algorithm>
#include <cstring>
#include <vector>

static bool AllPixelsBlack(const QImage& image)
{
	for (int y = 0; y < image.height(); ++y)
	{
		const auto row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
		for (int x = 0; x < image.width(); ++x)
			if (row[x] & 0x00ffffff) return false;
	}
	return true;
}

KRRFrameCapture::~KRRFrameCapture()
{
	if (m_previous)
		SelectObject(m_memory, m_previous);
	if (m_bitmap)
		DeleteObject(m_bitmap);
	if (m_memory)
		DeleteDC(m_memory);
	if (m_screen)
		ReleaseDC(nullptr, m_screen);
}
QRect KRRFrameCapture::VirtualDesktop()
{
	QRect desktop;
	for (const auto &value : Monitors())
	{
		const auto m = value.toObject();
		desktop = desktop.united(QRect(m["x"].toInt(), m["y"].toInt(), m["width"].toInt(), m["height"].toInt()));
	}
	return desktop;
}
struct MonitorList
{
	QJsonArray items;
	bool details = false;
	QHash<QString, QString> names;
};
static QHash<QString, QString> MonitorNames()
{
	QHash<QString, QString> names;
	for (int retry = 0; retry < 3; ++retry)
	{
		UINT32 paths = 0, modes = 0;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS)
			break;
		std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
		std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
		LONG result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(), nullptr);
		if (result == ERROR_INSUFFICIENT_BUFFER)
			continue;
		if (result != ERROR_SUCCESS)
			break;
		for (UINT32 i = 0; i < paths; ++i)
		{
			DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
			source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path[i].sourceInfo.adapterId,
							 path[i].sourceInfo.id};
			DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
			target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), path[i].targetInfo.adapterId,
							 path[i].targetInfo.id};
			if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
				DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
			{
				QString name = QString::fromWCharArray(target.monitorFriendlyDeviceName).trimmed();
				if (!name.isEmpty())
					names.insert(QString::fromWCharArray(source.viewGdiDeviceName), name);
			}
		}
		break;
	}
	return names;
}
static BOOL CALLBACK AddMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
	MONITORINFOEXW information{};
	information.cbSize = sizeof(information);
	if (!GetMonitorInfoW(monitor, &information))
		return TRUE;
	auto list = reinterpret_cast<MonitorList *>(parameter);
	RECT r = information.rcMonitor;
	QJsonObject item{{"name", QString::fromWCharArray(information.szDevice)},
					 {"x", static_cast<int>(r.left)},
					 {"y", static_cast<int>(r.top)},
					 {"width", static_cast<int>(r.right - r.left)},
					 {"height", static_cast<int>(r.bottom - r.top)},
					 {"primary", (information.dwFlags & MONITORINFOF_PRIMARY) != 0}};
	if (list->details)
	{
		QString device = QString::fromWCharArray(information.szDevice);
		QString friendly = list->names.value(device);
		if (friendly.isEmpty())
		{
			DISPLAY_DEVICEW display{};
			display.cb = sizeof(display);
			if (EnumDisplayDevicesW(information.szDevice, 0, &display, 0))
				friendly = QString::fromWCharArray(display.DeviceString);
		}
		item["friendly_name"] = friendly.isEmpty() ? device : friendly;
		auto color=ScreenColorMetadata(device);for(auto it=color.begin();it!=color.end();++it)item[it.key()]=it.value();
		using GetMonitorDpi = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
		static HMODULE shcore = LoadLibraryW(L"shcore.dll");
		static auto getDpi =
			shcore ? reinterpret_cast<GetMonitorDpi>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
		UINT dpiX = 96, dpiY = 96;
		if (getDpi)
			getDpi(monitor, 0, &dpiX, &dpiY);
		item["dpi_x"] = static_cast<int>(dpiX);
		item["dpi_y"] = static_cast<int>(dpiY);
		item["scale_percent"] = qRound(dpiX * 100.0 / 96.0);
	}
	if (information.dwFlags & MONITORINFOF_PRIMARY)
		list->items.prepend(item);
	else
		list->items.append(item);
	return TRUE;
}
QJsonArray KRRFrameCapture::Monitors()
{
	MonitorList list;
	list.details = true;
	list.names = MonitorNames();
	EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&list));
	return list.items;
}
QString KRRFrameCapture::MonitorDevice(int index)
{
	MonitorList list;
	EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&list));
	return list.items.at(index).toObject()["name"].toString();
}
QRect KRRFrameCapture::CaptureArea(const KRRRecordingConfig &config)
{
	if (config.source == "region")
		return config.region;
	if (config.source == "window")
		return KRRWindowCapture::Bounds(reinterpret_cast<HWND>(config.window));
	MonitorList list;
	EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&list));
	auto m = list.items.at(config.monitor).toObject();
	if (config.source == "screen" && !config.monitorDevice.isEmpty())
	{
		m = {};
		for (const auto& value : list.items)
		{
			const auto screen = value.toObject();
			if (screen["name"].toString() != config.monitorDevice) continue;
			m = screen;
			break;
		}
	}
	return QRect(m["x"].toInt(), m["y"].toInt(), m["width"].toInt(), m["height"].toInt());
}
QImage KRRFrameCapture::capture(const KRRRecordingConfig &config, QString &error, KRRRecordingTimings* timings)
{
	if (config.source == "image")
	{
		m_window.reset();
		m_info={{"api","image"}};
		const QSize size(config.width, config.height);
		if (m_imagePath != config.image || m_stillFrame.size() != size)
		{
			m_imagePath = config.image;
			m_stillFrame = QImage(config.image).convertToFormat(QImage::Format_RGB32)
				.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		}
		if (m_stillFrame.isNull()) error = "Cannot decode the selected image";
		return m_stillFrame;
	}
	if (!m_stillFrame.isNull()) { m_stillFrame = {}; m_imagePath.clear(); }
	const bool window = config.source == "window";
	QRect area;
	QImage windowFrame;
	if (window)
	{
		windowFrame = m_window.capture(reinterpret_cast<HWND>(config.window), area, config.cursor, error, timings);
		m_info = m_window.diagnostics();
		if (windowFrame.isNull()) return {};
	}
	else
	{
		m_window.reset();
		area = CaptureArea(config);
	}
	if (area.width() < 2 || area.height() < 2)
	{
		if (config.source == "screen" && !config.monitorDevice.isEmpty())
		{
			m_info = {{"source_status", "display_disconnected"}, {"device", config.monitorDevice}};
			error = "Capture display was disconnected";
		}
		else error = "Invalid capture rectangle";
		return {};
	}
	const bool drawCursor = config.cursor && !(window && m_info.value("native_cursor").toBool());
	const QSize outputSize(config.width, config.height);
	if (window && !drawCursor && !config.clicks)
		return windowFrame.scaled(outputSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
	const QSize bitmapSize = window ? windowFrame.size() : outputSize;
	const bool newDesktop = !window && (m_desktopArea != area || m_desktopOutput != bitmapSize);
	if (newDesktop)
	{
		m_desktopArea = area;
		m_desktopOutput = bitmapSize;
		m_preferGdi = false;
		m_checkDesktop = true;
	}
	const auto desktop = window || m_preferGdi ? QImage{} : m_desktop.capture(area, bitmapSize, config, timings);
	if (!window) m_info = m_desktop.diagnostics();
	if (!desktop.isNull() && desktop.format() == QImage::Format_RGBA64)
	{
		m_checkDesktop = false;
		return desktop;
	}
	// Compare backends once for a blank desktop, before the cursor can mask it.
	const bool compare = m_checkDesktop && !desktop.isNull() &&
		!m_info.value("protected_content_masked").toBool() && AllPixelsBlack(desktop);
	if (!desktop.isNull()) m_checkDesktop = false;
	if (!desktop.isNull() && !compare && !config.cursor && !config.clicks)
		return desktop;
	if (!m_screen)
		m_screen = GetDC(nullptr);
	if (!m_memory)
	{
		m_memory = CreateCompatibleDC(m_screen);
		if (m_memory) SetStretchBltMode(m_memory, COLORONCOLOR);
	}
	if (!m_screen || !m_memory)
	{
		error = "Windows could not capture this surface";
		return {};
	}
	if (m_size != bitmapSize)
	{
		if (m_previous)
			SelectObject(m_memory, m_previous);
		if (m_bitmap)
			DeleteObject(m_bitmap);
		BITMAPINFO information{};
		information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		information.bmiHeader.biWidth = bitmapSize.width();
		information.bmiHeader.biHeight = -bitmapSize.height();
		information.bmiHeader.biPlanes = 1;
		information.bmiHeader.biBitCount = 32;
		information.bmiHeader.biCompression = BI_RGB;
		m_bitmap = CreateDIBSection(m_screen, &information, DIB_RGB_COLORS, &m_pixels, nullptr, 0);
		if (!m_bitmap)
		{
			m_previous = nullptr;
			error = "Could not allocate capture bitmap";
			return {};
		}
		m_previous = SelectObject(m_memory, m_bitmap);
		m_size = bitmapSize;
	}
	BOOL captured = FALSE;
	if (window || (!desktop.isNull() && !compare))
	{
		KRRStageTimer copy(timings, KRRRecordingTimings::ImageCopies);
		const auto& sourceFrame = window ? windowFrame : desktop;
		std::memcpy(m_pixels, sourceFrame.constBits(), static_cast<size_t>(m_size.width()) * m_size.height() * 4);
		captured = TRUE;
	}
	else
	{
		captured = m_size == area.size() ?
			BitBlt(m_memory, 0, 0, m_size.width(), m_size.height(), m_screen, area.x(), area.y(), SRCCOPY | CAPTUREBLT) :
			StretchBlt(m_memory, 0, 0, m_size.width(), m_size.height(), m_screen, area.x(), area.y(),
				area.width(), area.height(), SRCCOPY | CAPTUREBLT);
		if (captured) GdiFlush();
		const bool blank = (compare || newDesktop) && captured &&
			AllPixelsBlack(QImage(static_cast<uchar*>(m_pixels), m_size.width(), m_size.height(), QImage::Format_RGB32));
		if (compare && (!captured || blank))
		{
			std::memcpy(m_pixels, desktop.constBits(), static_cast<size_t>(m_size.width()) * m_size.height() * 4);
			captured = TRUE;
			m_info["desktop_black"] = true;
		}
		else
		{
			m_info["api"] = "GDI fallback";
			if (compare) m_preferGdi = true;
			if (m_preferGdi) m_info["fallback_reason"] = "dxgi_blank_frame";
			if (blank) m_info["desktop_black"] = true;
		}
	}
	if (!captured)
	{
		error = "Windows could not capture this surface";
		return {};
	}
	if (drawCursor)
	{
		CURSORINFO cursor{};
		cursor.cbSize = sizeof(cursor);
		if (GetCursorInfo(&cursor) && cursor.flags == CURSOR_SHOWING)
		{
			ICONINFO icon{};
			if (GetIconInfo(cursor.hCursor, &icon))
			{
				double scaleX = static_cast<double>(m_size.width()) / area.width();
				double scaleY = static_cast<double>(m_size.height()) / area.height();
				DrawIconEx(m_memory, static_cast<int>((cursor.ptScreenPos.x - area.x() - icon.xHotspot) * scaleX),
						   static_cast<int>((cursor.ptScreenPos.y - area.y() - icon.yHotspot) * scaleY), cursor.hCursor,
						   std::max(1, static_cast<int>(GetSystemMetrics(SM_CXCURSOR) * scaleX)),
						   std::max(1, static_cast<int>(GetSystemMetrics(SM_CYCURSOR) * scaleY)), 0, nullptr,
						   DI_NORMAL);
				GdiFlush();
				if (icon.hbmMask)
					DeleteObject(icon.hbmMask);
				if (icon.hbmColor)
					DeleteObject(icon.hbmColor);
			}
		}
	}
	if (window) GdiFlush();
	QImage image(static_cast<uchar *>(m_pixels), m_size.width(), m_size.height(), QImage::Format_RGB32);
	QImage result;
	{
		KRRStageTimer copy(timings, KRRRecordingTimings::ImageCopies);
		result = image.copy();
	}
	if (config.clicks && (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
	{
		POINT point{};
		GetCursorPos(&point);
		QPainter painter(&result);
		QColor color(config.clickColor);
		color.setAlpha(110);
		painter.setBrush(color);
		painter.setPen(QPen(QColor(config.clickColor), 3));
		painter.drawEllipse(QPoint((point.x - area.x()) * m_size.width() / area.width(),
								   (point.y - area.y()) * m_size.height() / area.height()),
							config.clickRadius, config.clickRadius);
	}
	return result.scaled(config.width, config.height, Qt::IgnoreAspectRatio, Qt::FastTransformation);
}
