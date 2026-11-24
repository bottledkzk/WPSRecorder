#ifndef WPSRECORDER_CLIPEDITOR_H
#define WPSRECORDER_CLIPEDITOR_H
#include "chapternarration.h"
#include "editanalysis.h"
#include "editplayer.h"
#include "mediaeditor.h"
#include <QDialog>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include <optional>
class KRREditCanvas;
class KRREditTimeline;
struct KRREditClipMove;
class QMediaPlayer;
class QPushButton;
class QLabel;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QTabWidget;
class QListWidget;
class QPlainTextEdit;
class QSlider;
class QLineEdit;
class QTimer;
class QAction;
class KRRClipEditor : public QDialog
{
	Q_OBJECT
  public:
	explicit KRRClipEditor(QWidget *parent = nullptr);
	~KRRClipEditor() override;
	bool openFile(const QString &input, const QString &folder, qint64 at = -1);
	bool openSmartCutRequest(const QString& input, const QString& folder, qint64 at,
		const KRRSmartCutOptions& options, bool chapters, QWidget* candidateHost = nullptr);
	bool openSmartCutProjectRequest(const QString& path, const QString& input, const QString& folder,
		qint64 at, const KRRSmartCutOptions& options, bool chapters, QWidget* candidateHost);
	void openSequenceMontage(const QStringList& inputs, const QString& folder, const QString& goal);
	void interruptSmartCut();
	void setRecording(bool active);
	bool isBusy() const
	{
		return m_exporter.isBusy() || m_asr.isBusy() || m_asrInProgress;
	}
	QString inputFile() const
	{
		return m_project.input;
	}
	void reject() override;
	bool confirmApplicationClose();
	void cancelTasks();
	void showSubtitles();
	void showSubtitleWorkflow();
	void saveCurrentProject() { saveProject(); }
	void exportCurrentProject() { exportProject(); }
	void pausePlayback();
	bool openProjectFile(const QString& path, const QString& folder);
	bool canSwitchProject() { return confirmReplace(); }
	QString projectPath() const { return m_projectPath; }
	QStringList projectMaterials() const;
	const KRREditProject& currentProject() const { return m_project; }
	static QStringList draftFiles();
	bool releaseIdleSource(const QString &path);
	void discardReleasedSource(const QString& path);
  signals:
	void exported(const QString &output);
	void batchRequested();
	void smartCutInterrupted();
	void sourceChanged(const QString& input);
	void studioCutApplied();

  protected:
	void closeEvent(QCloseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

  private:
	friend class KRREditAcceptance;
	friend class KRRSmartCutAcceptance;
	friend class KRRAssistantAcceptance;
	void refresh();
	void refreshMediaControls();
	void checkpoint();
	void undo(bool redo = false);
	void select(qint64 begin, qint64 end);
	void seek(qint64 time, bool pause = true);
	void stepFrame(int direction);
	void togglePlayback();
	void syncAudio(bool seekAudio);
	void requestFrame();
	void editSubtitleStyle();
	void importAudio();
	void editStill(int index = -1);
	void previewStill(int index, bool playing);
	int nextStill(qint64 from) const;
	void clearStill();
	void applyRange(bool retain);
	void chooseTrack(int track);
	void editCaption(bool update);
	void chooseCaption(int index);
	void moveClip(const KRREditClipMove& move);
	void splitSelectedTrack();
	void addSubtitleTrack();
	void applyClipSelection(bool retain);
	void exportSelectedClip();
	void exportTimeline(const KRRTimelineExportOptions& options);
	void refreshClipActions();
	void seekTimeline(qint64 time);
	void playArrangedSegment(int index, qint64 source = -1);
	qint64 editPosition() const;
	void filterCaptions();
	void refreshChapters();
	void showDubbingWorkflow(bool chapter);
	void editChapter(bool rename);
	void changeProtectedRanges(bool protect);
	void showBatchCut();
	void showSmartCut();
	void runRequestedSmartCut();
	void reviewSmartCut(const KRRSmartCutOptions* request, bool chapters);
	void applyCut(const KRREditProject& candidate);
	void importCaptions(const QString &path, bool sourceTimes = false);
	void autoCaptions();
	void saveProject();
	void loadProject();
	bool writeProject(const QString& path);
	bool saveDraft();
	bool prepareClose();
	QString draftPath() const;
	void restoreDraft(const QString& input);
	void exportProject();
	void exportSubmission();
	void clearMedia();
	bool confirmReplace();
	KRREditProject m_project;
	QVector<KRREditProject> m_undo, m_redo;
	std::unique_ptr<KRREditProject> m_pendingProject;
	std::optional<KRRSmartCutOptions> m_requestedCut;
	QPointer<QWidget> m_candidateHost;
	bool m_requestedChapters = false;
	std::unique_ptr<QTemporaryDir> m_cache;
	KRREditAnalysis m_analysis;
	KRRMediaEditor m_exporter, m_asr;
	KRREditCanvas *m_canvas = nullptr;
	KRREditTimeline *m_timeline = nullptr;
	KRREditPlayer *m_player = nullptr;
	QVector<QMediaPlayer *> m_audioPlayers;
	QProcess m_frameProcess;
	QTimer m_frameTimer, m_stillTimer;
	int m_stillPreview = -1, m_nextStill = -1;
	QComboBox *m_cropRatio = nullptr;
	QSpinBox *m_typingCrop = nullptr;
	qint64 m_position = 0, m_begin = 0, m_end = 0, m_frameTime = 0;
	int m_arrangedSegment = -1;
	bool m_arrangedPlaying = false;
	qint64 m_openAt = -1;
	int m_track = -1, m_caption = -1;
	bool m_dirty = false, m_refreshing = false, m_atEnd = false;
	bool m_pendingSubtitleWorkflow = false, m_reopenAfterAsr = false, m_asrInProgress = false;
	bool m_returnToSmartCut = false;
	bool m_sourceReleased = false, m_sourceLoading = false;
	QList<QWidget*> m_mediaControls;
	QList<QAction*> m_mediaActions;
	QPushButton* m_chapterNarration = nullptr;
	QString m_folder, m_lastExport, m_asrRequestedLanguage, m_projectPath;
	QTimer m_draftTimer;
	QLabel *m_title = nullptr;
	QLabel *m_status = nullptr;
	QLabel *m_time = nullptr;
	QLabel *m_summary = nullptr;
	QPushButton *m_play = nullptr;
	QPushButton *m_undoButton = nullptr;
	QPushButton *m_redoButton = nullptr;
	QPushButton *m_result = nullptr;
	QPushButton *m_cropButton = nullptr;
	QComboBox *m_target = nullptr;
	QComboBox *m_asrTrack = nullptr;
	QComboBox *m_language = nullptr;
	QComboBox *m_asrModel = nullptr;
	QDialog *m_asrSettings = nullptr;
	QDoubleSpinBox *m_in = nullptr;
	QDoubleSpinBox *m_out = nullptr;
	QDoubleSpinBox *m_captionBegin = nullptr;
	QDoubleSpinBox *m_captionEnd = nullptr;
	QSpinBox *m_cropX = nullptr;
	QSpinBox *m_cropY = nullptr;
	QSpinBox *m_cropW = nullptr;
	QSpinBox *m_cropH = nullptr;
	QSpinBox *m_gain = nullptr;
	QCheckBox *m_cropPreview = nullptr;
	QCheckBox *m_trackEnabled = nullptr;
	QCheckBox *m_captionTrackVisible = nullptr;
	QTabWidget *m_inspector = nullptr;
	QTabWidget *m_assetsTabs = nullptr;
	QWidget *m_chapterPage = nullptr;
	KRRNarrationDucking m_narrationDucking;
	QListWidget *m_chapterList = nullptr;
	QListWidget *m_materials = nullptr, *m_stills = nullptr;
	QLineEdit *m_chapterTitle = nullptr;
	QListWidget *m_captionList = nullptr;
	QLineEdit *m_captionSearch = nullptr;
	QPlainTextEdit *m_captionText = nullptr;
	QSlider *m_zoom = nullptr;
	QSlider *m_scroll = nullptr;
	QSlider *m_previewSeek = nullptr;
	QPushButton *m_lockTrack = nullptr, *m_keepClip = nullptr, *m_deleteClip = nullptr, *m_exportClip = nullptr;
	QSet<int> m_lockedTracks;
	bool m_clipSelected = false, m_linkTracks = false;
};
#endif
