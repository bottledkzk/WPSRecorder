#ifndef WPSRECORDER_SEQUENCESMARTCUTDIALOG_H
#define WPSRECORDER_SEQUENCESMARTCUTDIALOG_H

#include "mediaeditor.h"
#include <QDialog>
#include <QTemporaryDir>

class KRREditCanvas;
class KRREditPlayer;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QMediaPlayer;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;

class KRRSequenceSmartCutDialog : public QDialog
{
public:
	explicit KRRSequenceSmartCutDialog(const KRREditSequence& source, QWidget* parent, const QString& goal = {});
	const KRREditSequence& result() const { return m_result; }
	void interrupt();
protected:
	void reject() override;
private:
	void analyze();
	void refreshRows(int selected);
	void refreshControls();
	void moveSelected(int delta);
	void editCut();
	void playEvidence();
	void previewMontage();
	void apply();
	void invalidatePreview();
	void play(const KRREditProject& view, qint64 start, qint64 end);
	void playRendered(const QString& path);
	void showUsage();
	void showProvider();

	KRREditSequence m_source, m_result;
	KRRSequenceSmartCutDraft m_draft;
	KRREditProject m_view;
	QVector<KRREditCaption> m_renderedCaptions;
	QTemporaryDir m_temporary;
	KRRMediaEditor m_worker;
	QString m_previewPath;
	int m_previewNumber = 0;
	qint64 m_playEnd = 0;
	bool m_audioStarted = false;
	KRREditCanvas* m_canvas;
	KRREditPlayer* m_player;
	QMediaPlayer* m_audio;
	QLineEdit* m_goal;
	QLabel *m_usage, *m_status, *m_provider, *m_clock, *m_selection;
	QSpinBox *m_targetDuration, *m_candidateCount, *m_sampleRate;
	QTableWidget* m_table;
	QDoubleSpinBox *m_begin, *m_end;
	QSlider* m_seek;
	QPushButton *m_send, *m_settings, *m_stop, *m_up, *m_down, *m_cut, *m_evidence, *m_preview, *m_pause, *m_apply;
};

#endif
