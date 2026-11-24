#ifndef WPSRECORDER_SPEECHSETTINGSDIALOG_H
#define WPSRECORDER_SPEECHSETTINGSDIALOG_H

#include <QString>

class QWidget;
class QComboBox;
void ConfigureSpeechServices(QWidget* owner, const QString& initial = "translate");
void RefreshTtsServiceChoices(QComboBox* engines);
bool ConfigureAudio8Directory(QWidget* owner);

#endif
