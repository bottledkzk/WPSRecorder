#ifndef WPSRECORDER_RECORDERTASKS_H
#define WPSRECORDER_RECORDERTASKS_H

#include <QJsonObject>
#include <atomic>

// Input and output come from the local caller, outside the model's request.
QJsonObject RunRecorderTask(const QJsonObject& request, const QString& input, const QString& output,
							const std::atomic<bool>* cancel);

#endif
