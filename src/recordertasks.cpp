#include "recordertasks.h"
#include "recordingassistant.h"
#include "controlserver.h"
#include "editproject.h"
#include "recordingmarkers.h"
#include "recordingrecovery.h"
#include "smartcut.h"
#include "sequencesmartcut.h"
#include "subtitlereview.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <cmath>

static QJsonObject Failure(const QString& code, const QString& detail = {})
{
	return {{"ok", false}, {"code", code}, {"detail", detail}};
}

static bool TimeOption(const QJsonObject& request, const QString& name, qint64 fallback, qint64& time)
{
	if (!request.contains(name)) { time = fallback; return true; }
	const double value = request[name].toDouble(-1);
	if (value < 0 || value > 864000000 || std::floor(value) != value) return false;
	time = static_cast<qint64>(value);
	return true;
}

static QJsonObject SubmissionTask(const QString& operation, const QJsonObject& arguments, const QString& input,
								  const QString& output, const std::atomic<bool>* cancel)
{
	for (auto i = arguments.begin(); i != arguments.end(); ++i)
		if (!QStringList{"begin_ms", "end_ms", "preview_at_ms", "preset", "audio", "audio_tracks"}.contains(i.key()))
			return Failure("invalid_argument", i.key());
	const auto preset = arguments.value("preset").toString(arguments.contains("preset") ? "" : "clear");
	const auto audio = arguments.value("audio").toString(arguments.contains("audio") ? "" : "mix");
	if ((preset != "clear" && preset != "small") || !QStringList{"mix", "separate", "mute"}.contains(audio))
		return Failure("invalid_submission_options");
	KRREditProject project;
	QString error;
	const auto file = QFileInfo(input);
	if (!file.isFile()) return Failure("media_required");
	if (!ProbeEditSource(file.absoluteFilePath(), project, error)) return Failure("probe_failed", error);
	qint64 begin = 0, end = 0, at = 0;
	if (!TimeOption(arguments, "begin_ms", 0, begin) ||
		!TimeOption(arguments, "end_ms", project.duration, end) ||
		!TimeOption(arguments, "preview_at_ms", begin, at) ||
		end > project.duration || end - begin < 20 || at < begin || at >= end)
		return Failure("invalid_range");
	project.kept = {{begin, end}};
	const bool selectTracks = arguments.contains("audio_tracks");
	if (selectTracks && !arguments["audio_tracks"].isArray()) return Failure("invalid_audio_tracks");
	QSet<int> selected;
	for (const auto value : arguments["audio_tracks"].toArray())
	{
		const int track = value.toInt(-1);
		if (track < 0 || track >= project.audio.size() || selected.contains(track))
			return Failure("invalid_audio_tracks");
		selected.insert(track);
	}
	QJsonArray tracks;
	int enabled = 0;
	for (int i = 0; i < project.audio.size(); ++i)
	{
		auto& track = project.audio[i];
		track.enabled = audio != "mute" && (!selectTracks || selected.contains(i));
		enabled += track.enabled;
		tracks.append(QJsonObject{{"index", i}, {"title", track.title}, {"enabled", track.enabled}});
	}
	const bool separate = audio == "separate";
	const int count = separate ? enabled : qMin(1, enabled);
	auto options = SubmissionPreset(preset);
	const auto size = options.outputSize(project.crop.size());
	const auto duration = project.outputDuration();
	const auto previewStart = qMin(at - begin, qMax<qint64>(0, duration - 8000));
	QJsonObject result{{"ok", true}, {"begin_ms", begin}, {"end_ms", end}, {"duration_ms", duration},
		{"preset", preset}, {"width", size.width()}, {"height", size.height()}, {"audio", audio},
		{"audio_tracks", tracks}, {"output_audio_tracks", count}, {"preview_begin_ms", begin + previewStart},
		{"video_kbps", options.videoKbps}, {"audio_kbps_per_track", options.audioKbps},
		{"estimated_bytes", duration * (options.videoKbps + count * options.audioKbps) / 8}};
	if (operation == "submission.plan") return result;

	if (output.isEmpty() || QFileInfo(output).suffix().toLower() != "mp4")
		return Failure("output_mp4_required");
	if (operation == "submission.preview")
	{
		options.previewStart = previewStart;
		options.previewLength = 8000;
		result["preview_duration_ms"] = qMin(options.previewLength, duration - previewStart);
	}
	const auto destination = QFileInfo(output).absoluteFilePath();
	error = RenderEditProject(project, destination, "auto", separate, true, cancel, options);
	if (!error.isEmpty()) return Failure("render_failed", error);
	result["bytes_written"] = QFileInfo(destination).size();
	return result;
}

static bool LoadTaskProject(const QString& input, KRREditProject& project, QString& error)
{
	if (QFileInfo(input).suffix().toLower() != "json") return ProbeEditSource(input, project, error);
	QFile file(input);
	if (!file.open(QIODevice::ReadOnly) || file.size() > 4000000) { error = "Could not open the edit project"; return false; }
	const auto doc = QJsonDocument::fromJson(file.readAll());
	return KRREditProject::FromJson(doc.object(), project, error);
}

static bool SaveTaskJson(const QString& output, const QJsonObject& object, QString& error)
{
	if (QFileInfo(output).suffix().toLower() != "json" || QFileInfo::exists(output))
	{
		error = "Choose a new .json destination";
		return false;
	}
	QSaveFile file(output);
	const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
	{
		error = "Could not write the JSON destination";
		return false;
	}
	return true;
}

static QJsonObject EditTask(const QString& task, const QJsonObject& args, const QString& input, const QString& output,
							const std::atomic<bool>* cancel)
{
	QString error;
	KRREditProject project;
	if (!QFileInfo(input).isFile()) return Failure("media_required");
	if (!LoadTaskProject(input, project, error)) return Failure("project_unavailable", error);
	if (task == "edit.chapters")
	{
		if (!args.isEmpty() || !output.isEmpty()) return Failure("invalid_argument");
		QJsonArray source, finished;
		for (const auto& chapter : project.chapters)
			source.append(QJsonObject{{"at_ms", chapter.at}, {"title", chapter.title}});
		for (const auto& chapter : project.outputChapters())
			finished.append(QJsonObject{{"at_ms", chapter.at}, {"title", chapter.title}});
		return {{"ok", true}, {"source", source}, {"output", finished}};
	}
	if (task == "edit.subtitle-search")
	{
		const auto query = args["query"].toString();
		if (args.size() != 1 || query.trimmed().isEmpty() || query.size() > 200) return Failure("invalid_query");
		QJsonArray matches;
		const auto indexes = SearchEditCaptions(project.captions, query);
		for (int index : indexes.mid(0, 100))
		{
			const auto& caption = project.captions[index];
			matches.append(QJsonObject{{"begin_ms", caption.begin}, {"end_ms", caption.end}, {"text", caption.text},
					{"translation", caption.translation}});
		}
		return {{"ok", true}, {"matches", matches}, {"count", indexes.size()}};
	}
	if (task == "edit.subtitle-review")
	{
		if (!output.isEmpty()) return Failure("output_not_supported");
		for (auto it = args.begin(); it != args.end(); ++it)
			if (!QStringList{"first", "last", "target", "goal"}.contains(it.key())) return Failure("invalid_argument", it.key());
		qint64 first = 0, last = 0;
		if (!TimeOption(args, "first", 1, first) || !TimeOption(args, "last", first, last) ||
			first < 1 || last < first || last > project.captions.size() ||
			(args.contains("target") && !args["target"].isString()) || (args.contains("goal") && !args["goal"].isString()))
			return Failure("invalid_review_options");
		const auto language = args["target"].toString(project.translationLanguage);
		KRRSubtitleReview review;
		error = ReviewSubtitles(project, static_cast<int>(first - 1), static_cast<int>(last - 1), language,
			args["goal"].toString(), review, cancel);
		if (!error.isEmpty()) return Failure("subtitle_review_failed", error);
		return {{"ok", true}, {"review", review.toJson(project)}};
	}
	if (task == "edit.apply-cut")
	{
		KRRSmartCutDraft draft;
		KRREditProject result;
		if (output.isEmpty() || args.size() != 1 || !args["draft"].isObject()) return Failure("draft_and_output_required");
		if (!KRRSmartCutDraft::fromJson(args["draft"].toObject(), draft, error) ||
			!ApplySmartCutDraft(project, draft, result, error)) return Failure("invalid_draft", error);
		if (!SaveTaskJson(output, result.toJson(), error)) return Failure("save_failed", error);
		return {{"ok", true}, {"output", output}, {"duration_ms", result.outputDuration()}, {"ranges", result.kept.size()}};
	}
	for (auto it = args.begin(); it != args.end(); ++it)
		if (!QStringList{"mode", "query", "begin_ms", "end_ms", "max_ms", "chapters"}.contains(it.key())) return Failure("invalid_argument", it.key());
	if (args.contains("chapters") && !args["chapters"].isBool()) return Failure("invalid_argument", "chapters");
	KRRSmartCutOptions options;
	options.mode = args["mode"].toString("highlights");
	options.query = args["query"].toString();
	if ((args.contains("query") && !args["query"].isString()) ||
		!TimeOption(args, "begin_ms", 0, options.scope.begin) ||
		!TimeOption(args, "end_ms", project.duration, options.scope.end) ||
		!TimeOption(args, "max_ms", options.mode == "range" ? 0 : 45000, options.maxDuration))
		return Failure("invalid_range");
	if (!output.isEmpty() && (QFileInfo(output).suffix().toLower() != "json" || QFileInfo::exists(output)))
		return Failure("invalid_draft_destination");
	KRRSmartCutDraft draft;
	error = SuggestSmartCut(project, options, draft, cancel);
	if (!error.isEmpty()) return Failure("smartcut_failed", error);
	draft.addChapters = args["chapters"].toBool();
	if (!output.isEmpty() && !SaveTaskJson(output, draft.toJson(), error)) return Failure("save_failed", error);
	return {{"ok", true}, {"draft", draft.toJson()}};
}

static QJsonObject SequenceTask(const QString& task, const QJsonObject& args, const QString& input,
	const QString& output, const std::atomic<bool>* cancel)
{
	if (!output.isEmpty() && (QFileInfo(output).suffix().toLower() != "json" || QFileInfo::exists(output)))
		return Failure("invalid_sequence_destination");
	QJsonObject json;
	KRREditSequence sequence;
	QString error;
	if (!ReadEditProjectJson(input, json, error) || !ReadEditSequence(json, sequence, error))
		return Failure("invalid_sequence", error);
	KRRSequenceSmartCutDraft draft;
	if (task == "sequence.apply-cut")
	{
		if (output.isEmpty() || args.size() != 1 || !args["draft"].isObject()) return Failure("draft_and_output_required");
		KRREditSequence result;
		if (!KRRSequenceSmartCutDraft::FromJson(args["draft"].toObject(), draft, error) ||
			!BuildSequenceSmartCut(sequence, draft, result, error)) return Failure("invalid_draft", error);
		if (!SaveEditSequence(output, result, error)) return Failure("save_failed", error);
		return {{"ok", true}, {"output", output}, {"duration_ms", EditSequenceDuration(result)}, {"parts", result.size()}};
	}
	if (!args["goal"].isString()) return Failure("goal_required");
	for (auto i = args.begin(); i != args.end(); ++i)
		if (!QStringList{"goal", "target_ms", "candidates_per_source", "sample_fps"}.contains(i.key()))
			return Failure("invalid_argument", i.key());
	KRRSequenceCutOptions options;
	qint64 count = 0, fps = 0;
	if (!TimeOption(args, "target_ms", options.targetMs, options.targetMs) || options.targetMs > MaxSequenceTargetMs ||
		!TimeOption(args, "candidates_per_source", options.candidatesPerSource, count) || count > MaxSequenceCandidates ||
		!TimeOption(args, "sample_fps", options.sampleFps, fps) || fps > MaxSequenceSampleFps)
		return Failure("invalid_sequence_cut_options");
	options.candidatesPerSource = static_cast<int>(count);
	options.sampleFps = static_cast<int>(fps);
	error = SuggestSequenceSmartCut(sequence, args["goal"].toString(), options, draft, cancel);
	if (!error.isEmpty())
	{
		auto response = Failure("sequence_cut_failed", error);
		response["usage"] = draft.toJson();
		return response;
	}
	if (!output.isEmpty() && !SaveTaskJson(output, draft.toJson(), error)) return Failure("save_failed", error);
	return {{"ok", true}, {"draft", draft.toJson()}};
}

QJsonObject RunRecorderTask(const QJsonObject& request, const QString& input, const QString& output,
							const std::atomic<bool>* cancel)
{
	if (request.value("version").toInt() != 1) return Failure("unsupported_version");
	for (auto i = request.begin(); i != request.end(); ++i)
		if (!QStringList{"version", "task", "args"}.contains(i.key())) return Failure("invalid_request", i.key());
	if (request.contains("args") && !request["args"].isObject()) return Failure("invalid_arguments");
	const auto task = request["task"].toString();
	const auto args = request["args"].toObject();
	if (task == "submission.plan" || task == "submission.preview" || task == "submission.export")
		return SubmissionTask(task, args, input, output, cancel);
	if (task == "edit.smart-cut" || task == "edit.apply-cut" || task == "edit.subtitle-search" ||
		task == "edit.subtitle-review" || task == "edit.chapters")
		return EditTask(task, args, input, output, cancel);
	if (task == "sequence.smart-cut" || task == "sequence.apply-cut")
		return SequenceTask(task, args, input, output, cancel);
	if (task == "recording.search" || task == "recording.diagnose" ||
		task == "recording.settings-preview" || task == "assistant.ask")
	{
		if (!output.isEmpty()) return Failure("output_not_supported");
		if (!input.isEmpty() && !QFileInfo(input).isDir()) return Failure("directory_required");
		const auto context = SavedAssistantContext(input);
		if (task == "recording.search") return FindAssistantRecordings(context, args, cancel);
		if (task == "recording.diagnose") return DiagnoseAssistantRecording(context, args);
		if (task == "recording.settings-preview")
		{
			if (args.size() != 1 || !args["changes"].isObject()) return Failure("changes_object_required");
			return PreviewAssistantSettings(context.settings, args["changes"].toObject());
		}
		if (args.size() != 1 || !args["prompt"].isString()) return Failure("prompt_required");
		return AskRecordingAssistant(args["prompt"].toString(), context, SpeechService("assistant"), {}, cancel).toJson();
	}
	if (!args.isEmpty()) return Failure("invalid_arguments");
	if (task == "recording.status")
	{
		QJsonObject status;
		QString error;
		if (!SendControl({{"command", "status"}}, status, error, 500))
			return Failure("service_unavailable", error);
		return {{"ok", true}, {"state", status["state"]}, {"phase", status["phase"]},
			{"pipeline_stage", status["pipeline_stage"]}, {"source", status["source"]}, {"paused", status["paused"]},
			{"duration_sec", status["duration_sec"]}, {"encoder", status["encoder"]},
			{"audio_mode", status["audio_mode"]}, {"mic_enabled", status["mic_enabled"]},
			{"capture", status["capture"].toObject()["api"]}, {"marker_count", status["marker_count"]},
			{"failure_stage", status["failure_stage"]}, {"output_published", status["output_published"]},
			{"has_recovery", !status["recovery_workspace"].toString().isEmpty()}};
	}
	if (task == "recording.markers")
	{
		if (!QFileInfo(input).isFile()) return Failure("media_required");
		return {{"ok", true}, {"markers", ReadRecordingMarkers(input)}};
	}
	if (task == "recording.recoveries")
	{
		if (input.isEmpty() || !QFileInfo(input).isDir()) return Failure("directory_required");
		QJsonArray entries;
		for (const auto& item : RecordingRecoveryItems(input))
			entries.append(QJsonObject{{"name", item.name}, {"stage", item.stage}, {"audio", QJsonArray::fromStringList(item.audio)},
				{"camera", item.hasCamera}, {"modified", item.modified.toString(Qt::ISODate)}});
		return {{"ok", true}, {"items", entries}};
	}
	return Failure("unknown_task");
}
