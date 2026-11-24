#ifndef WPSRECORDER_SUBTITLEDIALOG_H
#define WPSRECORDER_SUBTITLEDIALOG_H

#include "mediaeditor.h"
#include "subtitletext.h"
#include <QDialog>
#include <QTemporaryDir>
#include <memory>

class QComboBox;
class KRRCompatiblePreview;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QMediaPlayer;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QWidget;

class KRRSubtitleDialog : public QDialog
{
	Q_OBJECT
public:
	KRRSubtitleDialog(const KRREditProject& project, const QString& folder, QWidget* owner,
		bool chapter = false, qint64 position = 0);
	~KRRSubtitleDialog() override;
	const KRREditProject& draft() const { return m_project; }
	bool requestedAsr() const { return m_requestedAsr; }
	void reject() override;

signals:
	void reviewInterrupted();

private:
	enum class Job { Idle, Translate, Generate, Narrate, Build, VoiceSample };
	struct Take
	{
		QString path, text;
		qint64 duration = 0;
		bool temporary = false;
	};

	void refreshRow(int row);
	void loadRow(int row);
	void storeRow();
	void updateControls();
	void updateVoice();
	void translate(bool selected);
	void review();
	void editPronunciations();
	void editLinePronunciation();
	void filterVisibleRows();
	void importChapterScript();
	void resetRows();
	void invalidateStaleTake(int row);
	void generate(bool selected);
	void generateNarration();
	bool applyNarration();
	bool narrationMode() const;
	bool narrationReady() const;
	QString narrationText() const;
	void startNextTake();
	void completed(const QString& output, const QString& error);
	void play(bool original);
	void previewVoice();
	void playFile(const QString& file, qint64 begin = 0, qint64 end = -1);
	void updateAudition();
	void exportSubtitles();
	void buildTrack();
	bool saveDraft();
	QString spoken(int row) const;
	bool usableTake(int row) const;
	KRRVoiceOptions voiceOptions() const;
	void showStatus(const QString& message);
	int dubbingTrackIndex() const;
	QVector<KRREditCaption>& cues() { return m_chapterMode ? m_project.chapterNarration : m_project.captions; }
	const QVector<KRREditCaption>& cues() const { return m_chapterMode ? m_project.chapterNarration : m_project.captions; }

	const bool m_chapterMode;
	KRREditProject m_project;
	QString m_folder;
	QTemporaryDir m_cache;
	KRRMediaEditor m_worker;
	QVector<Take> m_takes;
	KRRPronunciations m_pronunciations;
	QVector<int> m_pending;
	QVector<int> m_translationRows;
	QVector<KRREditCaption> m_placedNarration;
	QVector<KRREditRange> m_placedSpeech;
	KRRVoiceOptions m_queuedVoice;
	KRRVoiceOptions m_narratedVoice;
	Take m_freeTake;
	QString m_sourceLanguage;
	QString m_voiceEngine, m_voiceLanguage, m_voiceRevision;
	QString m_narratedRevision, m_voiceSamplePath;
	int m_row = -1, m_queued = 0, m_long = 0;
	int m_waitingOriginalRow = -1;
	qint64 m_previewEnd = -1;
	bool m_loading = false, m_cancelled = false, m_requestedAsr = false;
	bool m_voiceAvailable = true;
	Job m_job = Job::Idle;
	QWidget* m_controls = nullptr;
	QTabWidget* m_sources = nullptr;
	QTableWidget* m_table = nullptr;
	QComboBox *m_direction = nullptr, *m_engine = nullptr, *m_voice = nullptr, *m_read = nullptr, *m_freeLanguage = nullptr;
	QComboBox* m_subtitleTrack = nullptr;
	QPlainTextEdit *m_original = nullptr, *m_translation = nullptr, *m_freeText = nullptr;
	QLineEdit *m_glossary = nullptr, *m_narration = nullptr, *m_search = nullptr;
	QDoubleSpinBox *m_speed = nullptr, *m_freeStart = nullptr;
	QSpinBox *m_duckPercent = nullptr, *m_dubVolume = nullptr;
	QLabel *m_status = nullptr, *m_voiceHint = nullptr;
	QPushButton *m_asr = nullptr, *m_translateAll = nullptr, *m_translateOne = nullptr, *m_make = nullptr, *m_batch = nullptr,
		*m_export = nullptr, *m_originalPlay = nullptr, *m_voicePlay = nullptr, *m_add = nullptr, *m_cancel = nullptr, *m_apply = nullptr;
	QPushButton *m_review = nullptr, *m_readings = nullptr;
	QPushButton* m_lineReading = nullptr;
	QPushButton *m_audio8Directory = nullptr, *m_makeNarration = nullptr, *m_voiceSample = nullptr;
	QMediaPlayer* m_player = nullptr;
	std::unique_ptr<KRRCompatiblePreview> m_originalPreview;
};

#endif
