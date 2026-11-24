#include "assistanthistory.h"

#include "recordingconfig.h"
#include "uilanguage.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>

namespace
{
double Now()
{
	return static_cast<double>(QDateTime::currentMSecsSinceEpoch());
}
}

KRRAssistantHistory::KRRAssistantHistory(QObject* parent)
	: QObject(parent), m_path(DataDirectory() + "/assistant-conversations.json")
{
	QFile file(m_path);
	if (!file.open(QIODevice::ReadOnly)) return;
	const auto data = QJsonDocument::fromJson(file.readAll()).object();
	for (const auto& entry : data["conversations"].toArray())
	{
		auto value = entry.toObject();
		if (!value["id"].toString().isEmpty() && value["group"].isString() && value["messages"].isArray())
			m_entries.append(value);
	}
}

QString KRRAssistantHistory::begin(const QString& group)
{
	const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	m_entries.append(QJsonObject{{"id", id}, {"group", group}, {"visited", Now()}, {"messages", QJsonArray{}}});
	// Empty sidebar conversations are session-local until the user enters a message.
	if (!group.startsWith("side:")) save();
	emit changed();
	return id;
}

int KRRAssistantHistory::indexOf(const QString& id) const
{
	for (int i = 0; i < m_entries.size(); ++i)
		if (m_entries[i].toObject()["id"] == id) return i;
	return -1;
}

QJsonObject KRRAssistantHistory::entry(const QString& id) const
{
	const int i = indexOf(id);
	return i >= 0 ? m_entries[i].toObject() : QJsonObject{};
}

QJsonArray KRRAssistantHistory::entries(bool archived) const
{
	QVector<QJsonObject> ordered;
	for (const auto& value : m_entries)
	{
		auto item = value.toObject();
		if (item["archived"].toBool() == archived) ordered.append(item);
	}
	std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b)
	{
		return a["visited"].toDouble() > b["visited"].toDouble();
	});
	QJsonArray result;
	for (const auto& item : ordered) result.append(item);
	return result;
}

void KRRAssistantHistory::visit(const QString& id)
{
	const int i = indexOf(id);
	if (i < 0) return;
	auto item = m_entries[i].toObject();
	item["visited"] = Now();
	item["unread"] = false;
	update(i, item);
}

void KRRAssistantHistory::append(const QString& id, const QString& role, const QString& text, bool unread)
{
	const int i = indexOf(id);
	if (i < 0 || text.isEmpty()) return;
	auto item = m_entries[i].toObject();
	auto messages = item["messages"].toArray();
	messages.append(QJsonObject{{"role", role}, {"content", text}});
	item["messages"] = messages;
	item["unread"] = unread;
	update(i, item);
}

void KRRAssistantHistory::setSummary(const QString& id, int through, const QString& summary)
{
	const int i = indexOf(id);
	if (i < 0 || summary.isEmpty()) return;
	auto item = m_entries[i].toObject();
	item["summary"] = summary;
	item["through"] = through;
	update(i, item);
}

void KRRAssistantHistory::setArchived(const QString& id, bool archived)
{
	const int i = indexOf(id);
	if (i < 0 || busy(id)) return;
	auto item = m_entries[i].toObject();
	item["archived"] = archived;
	update(i, item);
}

void KRRAssistantHistory::remove(const QString& id)
{
	const int i = indexOf(id);
	if (i < 0 || busy(id)) return;
	m_entries.removeAt(i);
	save();
	emit changed();
}

void KRRAssistantHistory::setBusy(const QString& id, bool active)
{
	if (id.isEmpty()) return;
	if (active) m_busy.insert(id);
	else m_busy.remove(id);
	emit changed();
}

QString KRRAssistantHistory::title(const QJsonObject& conversation)
{
	for (const auto& value : conversation["messages"].toArray())
		if (value.toObject()["role"] == "user")
			return value.toObject()["content"].toString().simplified().left(48);
	return Ui("新对话");
}

void KRRAssistantHistory::update(int index, const QJsonObject& item)
{
	m_entries.replace(index, item);
	save();
	emit changed();
}

void KRRAssistantHistory::save()
{
	QJsonArray durable;
	for (const auto& item : m_entries)
		if (!item.toObject()["group"].toString().startsWith("side:") || !item.toObject()["messages"].toArray().isEmpty())
			durable.append(item);
	const auto data = QJsonDocument(QJsonObject{{"version", 1}, {"conversations", durable}}).toJson(QJsonDocument::Compact);
	QSaveFile file(m_path);
	if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
		emit saveFailed(Ui("无法保存助手会话。"));
}
