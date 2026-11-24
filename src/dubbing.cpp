#include "dubbing.h"
#include "uilanguage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>

namespace
{
constexpr qint64 BytesPerMs = 96; // Mono PCM S16LE at 48 kHz.

struct Pcm
{
	qint64 position = 0, bytes = 0;
};

Pcm InspectWave(QFile& file)
{
	const auto header = file.read(12);
	if (header.size() != 12 || header.left(4) != "RIFF" || header.mid(8) != "WAVE") return {};
	bool typed = false;
	while (file.pos() <= file.size() - 8)
	{
		const auto chunk = file.read(8);
		if (chunk.size() != 8) return {};
		const auto kind = chunk.left(4);
		const qint64 length = qFromLittleEndian<quint32>(chunk.constData() + 4);
		const auto position = file.pos();
		if (length > file.size() - position) return {};
		if (kind == "fmt " && length >= 16)
		{
			const auto spec = file.read(16);
			if (spec.size() != 16) return {};
			const auto fmt = spec.constData();
			typed = qFromLittleEndian<quint16>(fmt) == 1 && qFromLittleEndian<quint16>(fmt + 2) == 1 &&
				qFromLittleEndian<quint32>(fmt + 4) == 48000 && qFromLittleEndian<quint16>(fmt + 14) == 16;
		}
		if (kind == "data") return typed && length % 2 == 0 ? Pcm{position, length} : Pcm{};
		if (!file.seek(position + length + (length & 1))) return {};
	}
	return {};
}

QByteArray WaveHeader(quint32 size)
{
	QByteArray data = QByteArray::fromHex("524946460000000057415645666d7420100000000100010080bb000000770100020010006461746100000000");
	qToLittleEndian<quint32>(size + 36, data.data() + 4);
	qToLittleEndian<quint32>(size, data.data() + 40);
	return data;
}
} // namespace

qint64 SpeechWaveDuration(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return 0;
	return (InspectWave(file).bytes + BytesPerMs - 1) / BytesPerMs;
}

QString MixDubbingTrack(qint64 duration, const QVector<KRRDubLine>& lines, const QString& output,
						const std::atomic<bool>* cancel)
{
	if (lines.isEmpty() || lines.size() > 5000 || duration <= 0 || duration > (0xffffffffLL - 36) / BytesPerMs)
		return Ui("配音轨道为空或太长，请分段生成。");
	if (QFileInfo::exists(output)) return Ui("目标文件已存在，请选择新文件名。");
	auto ordered = lines;
	std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
	const auto payload = duration * BytesPerMs;
	QTemporaryDir staging(QFileInfo(output).absolutePath() + "/.dubbing-XXXXXX");
	if (!staging.isValid()) return Ui("无法创建配音工作目录。");
	QFile wave(staging.filePath("dubbing.wav"));
	if (!wave.open(QIODevice::ReadWrite)) return Ui("无法保存配音文件。");
	const auto header = WaveHeader(static_cast<quint32>(payload));
	if (wave.write(header) != header.size() || !wave.resize(header.size() + payload))
		return Ui("无法保存配音文件。");
	qint64 previousEnd = 0;
	for (const auto& line : ordered)
	{
		if (cancel && cancel->load()) return Ui("任务已取消。");
		QFile voice(line.audio);
		if (!voice.open(QIODevice::ReadOnly)) return Ui("配音文件已移动或不可用。");
		const auto media = InspectWave(voice);
		if (line.begin < previousEnd || line.begin < 0 || line.end <= line.begin || line.end > duration ||
			media.bytes <= 0 || media.bytes > (line.end - line.begin) * BytesPerMs)
			return Ui("部分句子超出字幕时段或彼此重叠，请调整字幕或重做配音。");
		if (!voice.seek(media.position) || !wave.seek(header.size() + line.begin * BytesPerMs))
			return Ui("无法读取旁白音频。");
		for (qint64 left = media.bytes; left > 0;)
		{
			if (cancel && cancel->load()) return Ui("任务已取消。");
			const auto chunk = voice.read(qMin<qint64>(left, 65536));
			if (chunk.isEmpty() || wave.write(chunk) != chunk.size()) return Ui("无法保存配音文件。");
			left -= chunk.size();
		}
		previousEnd = line.begin + (media.bytes + BytesPerMs - 1) / BytesPerMs;
	}
	if (!wave.flush()) return Ui("无法保存配音文件。");
	wave.close();
	if (!wave.rename(output)) return Ui("无法保存配音文件。");
	return {};
}

QString ExportSubtitleFiles(const KRREditProject& project, const QString& folder)
{
	QString error;
	if (!project.validate(error)) return error;
	const auto original = project.outputCaptions();
	const auto translated = project.outputCaptions(KRRSubtitleMode::Translation);
	if (translated.isEmpty()) return Ui("请先翻译保留区间中的字幕。");
	const auto bilingual = project.outputCaptions(KRRSubtitleMode::Bilingual);
	QDir parent(QFileInfo(folder).absolutePath());
	const auto name = QFileInfo(folder).fileName();
	if (name.isEmpty() || !parent.exists() || QFileInfo::exists(folder))
		return Ui("请选择一个尚未存在的字幕输出文件夹。");
	QTemporaryDir staging(parent.filePath(".subtitles-XXXXXX"));
	if (!staging.isValid()) return Ui("无法创建字幕输出文件夹。");
	const auto lang = [](const QString& value, const QString& fallback) -> QString {
		return value == "zh" ? "zh" : value == "en" ? "en" : fallback;
	};
	auto from = lang(project.subtitleLanguage, "original");
	const auto target = lang(project.translationLanguage, "translated");
	if (from == target) from = "original";
	if (!WriteEditSubtitles(staging.filePath("subtitle-" + from + ".srt"), original, error) ||
		!WriteEditSubtitles(staging.filePath("subtitle-" + target + ".srt"), translated, error) ||
		!WriteEditSubtitles(staging.filePath("subtitle-bilingual.srt"), bilingual, error)) return error;
	if (!parent.rename(QFileInfo(staging.path()).fileName(), name)) return Ui("无法保存字幕输出文件夹。");
	return {};
}
