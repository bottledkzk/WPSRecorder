#ifndef WPSRECORDER_EDITSEQUENCEDIALOG_H
#define WPSRECORDER_EDITSEQUENCEDIALOG_H
#include "mediaeditor.h"
#include <QDialog>
#include <QHash>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMediaPlayer;
class QPushButton;
class QSlider;
class KRREditCanvas;
class KRREditPlayer;
class KRRSequenceSmartCutDialog;

class KRREditSequenceDialog : public QDialog
{
public:
	explicit KRREditSequenceDialog(const KRREditProject& current, const QString& folder, QWidget* parent = nullptr);
	void openProject(const QString& path);
	void importFiles(const QStringList& paths, const QString& montageGoal = {});
	void interruptSmartCut();
protected:
	void reject() override;
private:
	KRREditSequencePart* selected();
	KRREditSequence ordered() const;
	void appendPart(KRREditSequencePart part);
	void updateRow(QListWidgetItem* item);
	void selectionChanged();
	void seekPreview();
	void refreshControls();
	void moveSelected(int delta);
	void cutSelection(bool retain);
	void locateSelected();
	bool saveProject();
	bool confirmReplace();
	void exportSequence();
	void openSmartCut(const QString& goal = {});
	void runIo(std::function<void()> work, std::function<void()> finished);
	void setStatus(const QString& message);

	KRREditProject m_current, m_preview;
	QString m_folder, m_projectPath, m_lastExport;
	KRRMediaEditor m_exporter;
	QHash<int, KRREditSequencePart> m_parts;
	QHash<int, QVector<KRREditRange>> m_lastCut;
	int m_nextId = 0;
	bool m_dirty = false, m_ioBusy = false;
	bool m_previewFrameOnly = false, m_audioStarted = false;
	QListWidget* m_list;
	KRREditCanvas* m_canvas;
	KRREditPlayer* m_player;
	QMediaPlayer* m_audio;
	QSlider* m_seek;
	QDoubleSpinBox *m_begin, *m_end;
	QLabel *m_summary, *m_ranges, *m_time, *m_status;
	QPushButton *m_add, *m_addCurrent, *m_open, *m_save, *m_up, *m_down, *m_remove, *m_locate;
	QPushButton *m_play, *m_useBegin, *m_useEnd, *m_keep, *m_cut, *m_undo;
	QPushButton *m_export, *m_cancel, *m_viewExport;
	QPushButton *m_smartCut, *m_evidence;
	QCheckBox* m_separate;
	KRRSequenceSmartCutDialog* m_activeSmartCut = nullptr;
};
#endif
