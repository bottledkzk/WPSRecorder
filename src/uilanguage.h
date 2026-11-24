#ifndef WPSRECORDER_UILANGUAGE_H
#define WPSRECORDER_UILANGUAGE_H
#include <QCoreApplication>
#include <QJsonObject>
#include <QString>
#include <memory>
class QTranslator;

inline QString Ui(const char *source)
{
	return QCoreApplication::translate("Recorder", source);
}

QString ReadUiLanguage();
bool WriteUiLanguage(const QString &language);
QString CurrentUiLanguage();
QJsonObject UiMessages();
QString UiDiagnostic(const QString &detail);

// Translators must outlive the widgets that use them.
class KRRUiLanguage
{
public:
	KRRUiLanguage();
	~KRRUiLanguage();
	bool install(const QString &language);

private:
	std::unique_ptr<QTranslator> m_translator;
};
#endif
