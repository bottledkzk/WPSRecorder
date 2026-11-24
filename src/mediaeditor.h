// mediaeditor.h | 3226535826@qq.com | 2026-10-31 | Accurate export and normalized concatenation.
#ifndef WPSRECORDER_MEDIAEDITOR_H
#define WPSRECORDER_MEDIAEDITOR_H
#include <QObject>
#include <QPair>
#include <QStringList>
#include <atomic>
#include <functional>
#include <thread>
#include "editproject.h"
#include "editsequence.h"
#include "dubbing.h"
#include "speech.h"
#include "smartcut.h"
#include "sequencesmartcut.h"
#include "subtitlereview.h"

struct KRRExportOptions
{
	QString encoder = "auto";
	bool denoise = false;
	QString subtitles;
	bool copyStreams = false;
};

class KRRMediaEditor : public QObject
{
	Q_OBJECT
public:
	explicit KRRMediaEditor(QObject* parent = nullptr);
	~KRRMediaEditor() override;
	bool trim(const QString& input, const QString& output, double begin, double end);
	bool split(const QString& input, const QString& directory, const QList<QPair<double, double>>& intervals);
	bool merge(const QStringList& inputs, const QString& output, const KRRExportOptions& options = {});
	bool exportMedia(const QString& input, const QString& output, double begin, double end, const KRRExportOptions& options = {});
	bool transcribe(const QString& input, const QString& output, const QString& executable, const QString& model, const QString& language, int audioTrack = 0);
	bool renderTimeline(const KRREditProject& project, const QString& output, const QString& encoder = "auto",
						bool separateAudio = true, bool burnCaptions = true, const KRRTimelineExportOptions& options = {});
	bool renderSequence(const KRREditSequence& sequence, const QString& output, bool separateAudio, bool preview = false);
	bool synthesize(const QString& text,const QString& output,const KRRVoiceOptions& options);
	bool translateSubtitles(const QVector<QString>& source, const QString& target, const QString& glossary, int only = -1);
	bool reviewSubtitles(const KRREditProject& project, int first, int last, const QString& target, const QString& goal);
	bool buildDubbing(qint64 duration, const QVector<KRRDubLine>& lines, const QString& output);
	bool suggestSmartCut(const KRREditProject& project, const KRRSmartCutOptions& options);
	bool suggestSequenceCut(const KRREditSequence& sequence, const QString& goal, const KRRSequenceCutOptions& options);
	bool narrateProject(const KRREditProject& project,const QString& speech,const QString& output,double begin,double originalVolume);
	bool recoverRecording(const QString& workspace, const QString& output);
	bool isBusy() const;
	void cancel() { m_cancel.store(true); }
signals:
	void busyChanged();
	void completed(const QString& output, const QString& error);
	void progress(const QString& message);
	void translated(const QVector<QString>& lines);
	void subtitlesReviewed(const KRRSubtitleReview& review);
	void smartDrafted(const KRRSmartCutDraft& draft);
	void sequenceDrafted(const KRRSequenceSmartCutDraft& draft);
private:
	bool launch(const QString& output, std::function<QString()> task);
	std::thread m_thread;
	std::atomic<bool> m_busy{false};
	std::atomic<bool> m_cancel{false};
};
#endif
