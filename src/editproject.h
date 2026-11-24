#ifndef WPSRECORDER_EDITPROJECT_H
#define WPSRECORDER_EDITPROJECT_H
#include <QColor>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <optional>

struct KRREditRange
{
	qint64 begin = 0, end = 0;
};
QVector<KRREditRange> MergeEditRanges(QVector<KRREditRange> ranges);
bool EditRangesContain(const QVector<KRREditRange>& ranges, KRREditRange range);
struct KRREditSubtitleStyle
{
	QString font = "Microsoft YaHei";
	int pixels = 0; // Zero follows video height, as in existing projects.
	QColor color = Qt::white;
	bool valid() const;
	QJsonObject toJson() const;
	static KRREditSubtitleStyle fromJson(const QJsonObject &o);
};
struct KRREditCaption
{
	qint64 begin = 0, end = 0;
	QString text;
	QString translation;
	QString dubbingFile, dubbingText, narrationText;
	bool dubbingNeedsRefresh = false;
	qint64 chapterAt = -1;
	int track = 0;
	QString pronunciation;
	std::optional<KRREditSubtitleStyle> style;
	QString displayText() const;
};
enum class KRRSubtitleMode { Original, Translation, Bilingual };
constexpr int SubtitleTrackSelection(int index) { return -2 - index; }
constexpr int SelectedSubtitleTrack(int selection) { return -2 - selection; }
struct KRREditTrack
{
	struct Clip
	{
		qint64 begin = 0, end = 0, mediaStart = 0;
	};
	QString title;
	int stream = 0;
	bool enabled = true;
	double gain = 1;
	QVector<KRREditRange> kept;
	QString file;
	qint64 offset = 0, length = 0;
	bool sentenceDubbing = false;
	bool needsRefresh = false;
	bool chapterDubbing = false;
	QVector<KRREditRange> speech;
	double originalGain = 1;
	QVector<Clip> placed;
	bool audible() const { return enabled && !needsRefresh; }
};
struct KRREditStill
{
	qint64 at = 0, length = 3000;
	QString image, caption; // Empty image uses the bundled art.
};
struct KRREditSegment
{
	qint64 begin, end;
	int still = -1;
};
struct KRREditChapter
{
	qint64 at = 0;
	QString title;
};
struct KRRTimelineExportOptions
{
	int maxHeight = 0;
	int videoKbps = 6000;
	int audioKbps = 192;
	qint64 previewStart = 0;
	qint64 previewLength = 0;
	QSize sequenceCanvas;
	double sequenceFps = 0;
	QVector<int> sequenceAudio; // Stage tracks in slot order. A negative index denotes silence.
	QSize outputSize(const QSize& crop) const;
};
KRRTimelineExportOptions SubmissionPreset(const QString& preset);
struct KRREditProject
{
	QString input;
	qint64 sourceBytes = 0, duration = 0;
	double fps = 30;
	QSize size;
	QRect crop;
	QSize cropRatio;
	QVector<KRREditRange> kept;
	QVector<KRREditRange> orderedVideo;
	QVector<KRREditRange> protectedRanges;
	QVector<qint64> splits;
	QVector<KRREditTrack> audio;
	QVector<KRREditCaption> captions;
	QVector<KRREditCaption> chapterNarration;
	QJsonObject chapterDrafts;
	QString chapterNarrationLanguage;
	QString subtitleLanguage, translationLanguage;
	QString subtitleGlossary, pronunciationGuide;
	KRREditSubtitleStyle subtitleStyle;
	QHash<int, KRREditSubtitleStyle> subtitleTrackStyles;
	QVector<QPointF> subtitlePositions{{.5, .9375}};
	QSet<int> hiddenSubtitleTracks;
	QVector<KRREditStill> stills;
	QVector<KRREditChapter> chapters;
	QStringList trackOrder;
	QVector<KRREditSegment> segments() const;
	QVector<KRREditRange> videoClips() const;
	QVector<KRREditTrack::Clip> audioClips(int track, bool sourceCoordinates = false) const;
	bool arranged() const { return !orderedVideo.isEmpty(); }
	void arrange();
	qint64 editDuration() const;
	qint64 outputTime(qint64 sourceTime) const;
	bool editArrangedVideo(qint64 begin, qint64 end, bool retain, QString& error);
	qint64 outputDuration() const;
	qint64 nextKept(qint64 sourceTime) const;
	bool editRange(qint64 begin, qint64 end, int track, bool retain, QString &error);
	bool editVideoRanges(const QVector<KRREditRange>& ranges, bool retain, QString& error);
	QVector<KRREditCaption> outputCaptions(KRRSubtitleMode mode = KRRSubtitleMode::Original, bool includeHidden = false) const;
	KRREditSubtitleStyle styleFor(const KRREditCaption& cue) const;
	QVector<KRREditCaption> captionsAt(qint64 time) const;
	bool captionFits(const KRREditCaption& caption, int ignored = -1) const;
	QVector<KRREditChapter> outputChapters() const;
	bool addChapter(qint64 at, const QString& title, QString& error);
	bool dubbingTrackNeedsRefresh(bool chapter = false) const;
	void markDubbingTrackStale(bool chapter = false);
	void setTrackOrder(const QJsonArray& order);
	QJsonObject toJson() const;
	static bool FromJson(const QJsonObject &object, KRREditProject &project, QString &error);
	bool validate(QString &error) const;
};
QRect FitEditCrop(QRect desired, QSize frame, QSize ratio, bool byHeight = false, bool right = false, bool bottom = false);
bool ProbeEditAudio(const QString &file, qint64 offset, qint64 duration, KRREditTrack &track, QString &error);
QImage LoadEditStill(const QString &image);
bool ReadEditSubtitleStyle(const QString &file, KRREditSubtitleStyle &style, QString &error);
bool ProbeEditSource(const QString &input, KRREditProject &project, QString &error, QJsonObject *container = nullptr);
bool ReadEditSubtitles(const QString &file, QVector<KRREditCaption> &captions, QString &error);
QStringList AdjacentEditSubtitlePaths(const QString& input);
QVector<KRREditCaption> ReadAdjacentEditSubtitles(const QString& input, qint64 duration);
bool WriteEditSubtitles(const QString &file, const QVector<KRREditCaption> &captions, QString &error);
bool WriteEditChapters(const QString &file, const QVector<KRREditChapter> &chapters, qint64 duration);
QString EditTime(qint64 milliseconds);
// Export one source non-destructively: ordered kept ranges, crop, captions and each audio track.
QString RenderEditProject(const KRREditProject &project, const QString &output, const QString &encoder = "auto",
						  bool separateAudio = true, bool burnCaptions = true,
						  const std::atomic<bool> *cancel = nullptr, const KRRTimelineExportOptions& options = {});
#endif
