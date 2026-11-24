#ifndef WPSRECORDER_ASSISTANTHISTORY_H
#define WPSRECORDER_ASSISTANTHISTORY_H

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>

class KRRAssistantHistory : public QObject
{
	Q_OBJECT
public:
	explicit KRRAssistantHistory(QObject* parent = nullptr);
	QString begin(const QString& group);
	QJsonObject entry(const QString& id) const;
	QJsonArray entries(bool archived = false) const;
	void visit(const QString& id);
	void append(const QString& id, const QString& role, const QString& text, bool unread = false);
	void setSummary(const QString& id, int through, const QString& summary);
	void setArchived(const QString& id, bool archived);
	void remove(const QString& id);
	bool busy(const QString& id) const { return m_busy.contains(id); }
	void setBusy(const QString& id, bool active);
	static QString title(const QJsonObject& conversation);

signals:
	void changed();
	void saveFailed(const QString& text);

private:
	int indexOf(const QString& id) const;
	void update(int index, const QJsonObject& item);
	void save();
	QJsonArray m_entries;
	QSet<QString> m_busy;
	QString m_path;
};
#endif
