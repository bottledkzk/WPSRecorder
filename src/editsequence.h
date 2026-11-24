#ifndef WPSRECORDER_EDITSEQUENCE_H
#define WPSRECORDER_EDITSEQUENCE_H
#include "editproject.h"
#include <functional>
#include <optional>

struct KRREditSequencePart
{
	QJsonObject saved;
	std::optional<KRREditProject> edit;
	QStringList unavailable;

	QString source() const;
	QJsonObject snapshot() const;
};

using KRREditSequence = QVector<KRREditSequencePart>;
constexpr int MaxSequenceParts = 128;
struct KRRSequenceExportOptions
{
	int videoKbps = 6000;
	bool burnCaptions = false;
};

bool ReadEditProjectJson(const QString& path, QJsonObject& json, QString& error);
bool MakeSequencePart(const KRREditProject& edit, KRREditSequencePart& part, QString& error);
bool ImportSequencePart(const QString& path, KRREditSequencePart& part, QString& error);
bool ReadEditSequence(const QJsonObject& json, KRREditSequence& sequence, QString& error);
bool SaveEditSequence(const QString& path, const KRREditSequence& sequence, QString& error);
QStringList UnavailableSequenceFiles(const KRREditSequencePart& part);
bool RelocateSequenceFile(KRREditSequencePart& part, const QString& oldPath, const QString& newPath, QString& error);
qint64 EditSequenceDuration(const KRREditSequence& sequence);
QString RenderEditSequence(const KRREditSequence& sequence, const QString& output, const QString& encoder = "auto",
	bool separateAudio = true, const std::atomic<bool>* cancel = nullptr,
	const std::function<void(int, int)>& progress = {}, bool preview = false, const KRRSequenceExportOptions& requested = {});
#endif
