#include "uilanguage.h"
#include "recordingconfig.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSettings>
#include <QTranslator>
#include <cstring>

namespace
{
QString g_language = "zh-CN";
constexpr auto PreferenceKey = "interfaceLanguage";
QString SettingsFile()
{
	return DataDirectory() + "/ui-settings.ini";
}
QJsonObject ReadCatalog(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	return QJsonDocument::fromJson(file.readAll()).object();
}
const QJsonObject& EnglishMessages()
{
	static const auto messages = ReadCatalog(":/i18n/en.json");
	return messages;
}
class EnglishCatalog final : public QTranslator
{
public:
	bool isEmpty() const override { return EnglishMessages().isEmpty(); }
	QString translate(const char *context, const char *source, const char *, int) const override
	{
		if (!context || !source || std::strcmp(context, "Recorder") != 0)
			return {};
		return EnglishMessages().value(QString::fromUtf8(source)).toString();
	}
};
}

QString ReadUiLanguage()
{
	QSettings settings(SettingsFile(), QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	return settings.value(PreferenceKey).toString() == "en" ? "en" : "zh-CN";
}
bool WriteUiLanguage(const QString &language)
{
	if (language != "en" && language != "zh-CN") return false;
	QSettings settings(SettingsFile(), QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	settings.setValue(PreferenceKey, language);
	settings.sync();
	return settings.status() == QSettings::NoError;
}
QString CurrentUiLanguage()
{
	return g_language;
}
QJsonObject UiMessages()
{
	return CurrentUiLanguage() == "en" ? EnglishMessages() : QJsonObject{};
}
KRRUiLanguage::KRRUiLanguage()
{
	Q_INIT_RESOURCE(ui_translations);
}
KRRUiLanguage::~KRRUiLanguage()
{
	if (m_translator) QCoreApplication::removeTranslator(m_translator.get());
}
bool KRRUiLanguage::install(const QString &language)
{
	std::unique_ptr<QTranslator> translator;
	if (language == "en")
		translator = std::make_unique<EnglishCatalog>();
	else if (language == "zh-CN")
	{
		translator = std::make_unique<QTranslator>();
		if (!translator->load(":/i18n/qtbase_zh_CN.qm")) return false;
	}
	else return false;
	if (!QCoreApplication::installTranslator(translator.get())) return false;
	if (m_translator) QCoreApplication::removeTranslator(m_translator.get());
	m_translator = std::move(translator);
	g_language = language;
	QLocale::setDefault(language == "en" ? QLocale(QLocale::English, QLocale::UnitedStates)
										 : QLocale(QLocale::Chinese, QLocale::China));
	return true;
}
QString UiDiagnostic(const QString &detail)
{
	if (detail.isEmpty()) return {};
	if (CurrentUiLanguage() == "en")
		return Ui(detail.toUtf8().constData());
	static const auto messages = ReadCatalog(":/i18n/diagnostics.zh-CN.json");
	const auto exact = messages.value(detail).toString();
	if (!exact.isEmpty()) return exact;
	// Keep filenames, Win32 codes and other details following a known message.
	for (auto i = messages.begin(); i != messages.end(); ++i)
		if (i.key().endsWith(' ') && detail.startsWith(i.key()))
			return i.value().toString() + detail.mid(i.key().size());
	return detail;
}
