#include "chronicle_helpers.h"

#include <algorithm>
#include <cmath>
#include <random>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace chronicle {

namespace {
QByteArray quoteJsonString(const QString& value) {
    return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)
        .mid(1)
        .chopped(1);
}
}

const QHash<QString, QString>& aliasMap() {
    static const QHash<QString, QString> m = {
        {"application/x-pdf",        "application/pdf"},
        {"image/jpg",                "image/jpeg"},
        {"audio/mp3",                "audio/mpeg"},
        {"text/javascript",          "application/javascript"},
        {"application/x-javascript", "application/javascript"},
        {"text/x-markdown",          "text/markdown"},
    };
    return m;
}

const QHash<QString, QString>& extensionMap() {
    static const QHash<QString, QString> m = {
        {"application/pdf",  "pdf"},
        {"text/plain",       "txt"},
        {"text/markdown",    "md"},
        {"text/csv",         "csv"},
        {"text/html",        "html"},
        {"application/json", "json"},
        {"application/zip",  "zip"},
        {"application/octet-stream", "bin"},
        {"image/jpeg",       "jpg"},
        {"image/png",        "png"},
        {"image/gif",        "gif"},
        {"image/webp",       "webp"},
        {"audio/mpeg",       "mp3"},
        {"audio/ogg",        "ogg"},
        {"video/mp4",        "mp4"},
        {"video/webm",       "webm"},
    };
    return m;
}

QString normalizeContentType(const QString& input) {
    QString s = input.trimmed().toLower();
    const int semi = s.indexOf(';');
    if (semi >= 0) {
        s = s.left(semi).trimmed();
    }
    if (s.size() > MAX_CT_LEN) {
        s = s.left(MAX_CT_LEN);
    }

    auto it = aliasMap().find(s);
    if (it != aliasMap().end()) {
        return it.value();
    }

    const int slash = s.indexOf('/');
    if (s.isEmpty() || slash <= 0 || slash == s.size() - 1) {
        return QStringLiteral("application/octet-stream");
    }
    return s;
}

QString sanitizeTitle(const QString& input) {
    QString s = input.trimmed();
    s.replace('/', '_').replace('\\', '_');
    if (s.size() > MAX_TITLE_LEN) {
        s = s.left(MAX_TITLE_LEN);
    }
    return s;
}

QString sanitizeDescription(const QString& input) {
    QString s = input.trimmed();
    if (s.size() > MAX_DESCRIPTION_LEN) {
        s = s.left(MAX_DESCRIPTION_LEN);
    }
    return s;
}

QString sanitizeTag(const QString& input) {
    QString s = input.trimmed();
    if (s.size() > MAX_TAG_LEN) {
        s = s.left(MAX_TAG_LEN);
    }
    return s;
}

QStringList normalizeTags(const QStringList& input) {
    QStringList out;
    out.reserve(std::min<int>(input.size(), MAX_TAGS));
    for (const QString& raw : input) {
        if (out.size() >= MAX_TAGS) {
            break;
        }
        const QString tag = sanitizeTag(raw);
        if (!tag.isEmpty()) {
            out.append(tag);
        }
    }
    return out;
}

QString extensionFor(const QString& contentType) {
    auto it = extensionMap().find(contentType);
    return it != extensionMap().end() ? it.value() : QString();
}

QString stripExtension(const QString& title, const QString& ext) {
    if (ext.isEmpty()) {
        return title;
    }
    const QString suffix = QStringLiteral(".") + ext;
    if (title.size() > suffix.size() &&
        title.right(suffix.size()).toLower() == suffix) {
        return title.left(title.size() - suffix.size());
    }
    return title;
}

QString synthesizeFilename(const QString& sanitizedTitle,
                           const QString& contentType) {
    const QString ext = extensionFor(contentType);
    const QString base = stripExtension(sanitizedTitle, ext);
    return ext.isEmpty() ? base : base + QStringLiteral(".") + ext;
}

QByteArray canonicalMetadataJson(const QString& contentType,
                                 qint64 sizeBytes,
                                 const QString& title,
                                 const QString& description,
                                 const QStringList& tags) {
    const QString normalizedCt = normalizeContentType(contentType);
    const QString normalizedTitle = sanitizeTitle(title);
    const QString normalizedDescription = sanitizeDescription(description);
    const QStringList normalizedTags = normalizeTags(tags);

    QByteArray json;
    json += "{\"content_type\":";
    json += quoteJsonString(normalizedCt);
    json += ",\"description\":";
    json += quoteJsonString(normalizedDescription);
    json += ",\"size_bytes\":";
    json += QByteArray::number(std::max<qint64>(0, sizeBytes));
    json += ",\"tags\":[";
    for (int i = 0; i < normalizedTags.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        json += quoteJsonString(normalizedTags[i]);
    }
    json += "],\"title\":";
    json += quoteJsonString(normalizedTitle);
    json += "}";
    return json;
}

QString hashMetadata(const QString& contentType,
                     qint64 sizeBytes,
                     const QString& title,
                     const QString& description,
                     const QStringList& tags) {
    const QByteArray canonical =
        canonicalMetadataJson(contentType, sizeBytes, title, description, tags);
    const QString hex = QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
    return QStringLiteral("v%1:%2").arg(METADATA_HASH_VERSION).arg(hex);
}

QJsonObject buildMetadataEnvelope(const QString& cid,
                                  const QString& contentType,
                                  qint64 sizeBytes,
                                  qint64 timestamp,
                                  const QString& title,
                                  const QString& description,
                                  const QStringList& tags,
                                  const QString& metadataHash) {
    QJsonObject envelope;
    envelope.insert(QStringLiteral("v"), 1);
    envelope.insert(QStringLiteral("cid"), cid);
    envelope.insert(QStringLiteral("content_type"), normalizeContentType(contentType));
    envelope.insert(QStringLiteral("size_bytes"), sizeBytes);
    envelope.insert(QStringLiteral("timestamp"), timestamp);
    envelope.insert(QStringLiteral("title"), sanitizeTitle(title));
    envelope.insert(QStringLiteral("metadata_hash"), metadataHash);

    const QString normalizedDescription = sanitizeDescription(description);
    envelope.insert(QStringLiteral("description"), normalizedDescription);

    const QStringList normalizedTags = normalizeTags(tags);
    QJsonArray tagArray;
    for (const QString& tag : normalizedTags) {
        tagArray.append(tag);
    }
    envelope.insert(QStringLiteral("tags"), tagArray);

    return envelope;
}

bool envelopeWithinCap(const QJsonObject& envelope) {
    const QByteArray compact =
        QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    return compact.size() <= MAX_ENVELOPE_BYTES;
}

bool isTransientError(const QString& msg) {
    const QString m = msg.toLower();
    if (m.contains(QStringLiteral("invalid")) ||
        m.contains(QStringLiteral("validation")) ||
        m.contains(QStringLiteral("rejected")) ||
        m.contains(QStringLiteral("malformed")) ||
        m.contains(QStringLiteral("not allowed"))) {
        return false;
    }
    return true;
}

std::chrono::milliseconds computeBackoff(int attempt) {
    int shift = std::min(attempt - 1, 5);
    int seconds = 1 << shift;
    if (seconds > 30) {
        seconds = 30;
    }

    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> jitter(0.75, 1.25);
    auto ms = static_cast<int64_t>(seconds * 1000.0 * jitter(rng));
    return std::chrono::milliseconds(ms);
}

qint64 uploadAttemptTimeoutMs(qint64 sizeBytes) {
    const qint64 mib = static_cast<qint64>(
        std::ceil(static_cast<double>(std::max<qint64>(0, sizeBytes)) /
                  static_cast<double>(1024 * 1024)));
    const qint64 scaled = BASE_ATTEMPT_TIMEOUT_MS +
                          mib * PER_MIB_ATTEMPT_TIMEOUT_MS;
    return std::clamp(scaled, BASE_ATTEMPT_TIMEOUT_MS,
                      MAX_ATTEMPT_TIMEOUT_MS);
}

qint64 uploadRetryBudgetMs(qint64 sizeBytes) {
    const qint64 scaled = uploadAttemptTimeoutMs(sizeBytes) * 3;
    return std::clamp(scaled, MIN_RETRY_BUDGET_MS,
                      MAX_RETRY_BUDGET_MS);
}

LogosResult failure(const QString& code, const QString& msg) {
    return {false, code, msg};
}

}  // namespace chronicle
