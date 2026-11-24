#ifndef WPSRECORDER_RECORDINGHOTKEYS_H
#define WPSRECORDER_RECORDINGHOTKEYS_H

#include "uilanguage.h"
#include <QHash>
#include <QKeySequence>
#include <QStringList>
#include <windows.h>

namespace RecordingHotkeys
{
inline constexpr int FirstId = 400;
inline constexpr int Minimal = 8;
inline constexpr int Count = Minimal + 1;

inline QStringList FromSettings(QStringList keys)
{
	while (keys.size() < 7) keys << QString();
	if (keys.size() == 7) keys << "Ctrl+Alt+M";
	if (keys.size() == Minimal) keys << (keys.contains("Ctrl+Alt+I", Qt::CaseInsensitive) ? QString{} : QStringLiteral("Ctrl+Alt+I"));
	return keys.mid(0, Count);
}

inline bool Binding(const QKeySequence& sequence, UINT& modifiers, UINT& nativeKey)
{
	if (sequence.count() != 1) return false;
	const int stroke = sequence[0];
	const int key = stroke & ~Qt::KeyboardModifierMask;
	modifiers = MOD_NOREPEAT;
	if (stroke & Qt::CTRL) modifiers |= MOD_CONTROL;
	if (stroke & Qt::ALT) modifiers |= MOD_ALT;
	if (stroke & Qt::SHIFT) modifiers |= MOD_SHIFT;
	if (stroke & Qt::META) modifiers |= MOD_WIN;
	if (!(modifiers & (MOD_ALT | MOD_CONTROL | MOD_WIN))) return false;
	static const QHash<int, UINT> special{
		{Qt::Key_Left, VK_LEFT}, {Qt::Key_Right, VK_RIGHT}, {Qt::Key_Up, VK_UP}, {Qt::Key_Down, VK_DOWN},
		{Qt::Key_Backspace, VK_BACK}, {Qt::Key_Tab, VK_TAB}, {Qt::Key_Backtab, VK_TAB},
		{Qt::Key_Return, VK_RETURN}, {Qt::Key_Enter, VK_RETURN}, {Qt::Key_Escape, VK_ESCAPE},
		{Qt::Key_Insert, VK_INSERT}, {Qt::Key_Delete, VK_DELETE}, {Qt::Key_Home, VK_HOME},
		{Qt::Key_End, VK_END}, {Qt::Key_PageUp, VK_PRIOR}, {Qt::Key_PageDown, VK_NEXT},
		{Qt::Key_Space, VK_SPACE}, {Qt::Key_Print, VK_SNAPSHOT}, {Qt::Key_Pause, VK_PAUSE}
	};
	if (key >= Qt::Key_F1 && key <= Qt::Key_F24) nativeKey = VK_F1 + key - Qt::Key_F1;
	else if (special.contains(key)) nativeKey = special.value(key);
	else if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)) nativeKey = key;
	else if (key > 0 && key <= 0xffff)
	{
		const auto binding = VkKeyScanW(static_cast<WCHAR>(key));
		if (binding == -1 || (HIBYTE(binding) & ~1)) return false;
		nativeKey = LOBYTE(binding);
		if (HIBYTE(binding) & 1) modifiers |= MOD_SHIFT;
	}
	else return false;
	return nativeKey != 0;
}

inline bool Register(HWND window, int action, const QString& key)
{
	UINT modifiers = 0, nativeKey = 0;
	return Binding(QKeySequence(key, QKeySequence::PortableText), modifiers, nativeKey)
		&& RegisterHotKey(window, FirstId + action, modifiers, nativeKey);
}
}
#endif
