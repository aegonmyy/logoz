#include "chronicle_anchor_client.h"

#include <QDebug>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>

#include <dlfcn.h>

namespace {
const char* kLib    = "libchronicle_registry_ffi.so";
const char* kEnvKey = "CHRONICLE_FFI_PATH";

QString errJson(const QString& msg) {
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{
            {QStringLiteral("ok"),    false},
            {QStringLiteral("error"), msg},
        }).toJson(QJsonDocument::Compact));
}

// Marker whose address dladdr maps back to chronicle_plugin.so, letting us
// locate the install directory to find the FFI sibling .so.
static void anchor_client_marker() {}

QString pluginDir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&anchor_client_marker), &info)
        && info.dli_fname)
        return QFileInfo(QString::fromUtf8(info.dli_fname)).absolutePath();
    return {};
}
} // namespace

FfiClient::FfiClient(QObject* parent) : QObject(parent) {}

FfiClient::~FfiClient() {
    if (m_lib.isLoaded()) m_lib.unload();
}

bool FfiClient::load(QString* err) {
    if (m_loaded) return true;

    // Determine .so path
    const QString envPath =
        QProcessEnvironment::systemEnvironment().value(QLatin1String(kEnvKey));
    QString libPath;
    if (!envPath.isEmpty()) {
        libPath = envPath;
    } else {
        const QString dir = pluginDir();
        if (!dir.isEmpty()) {
            const QString candidate = dir + QLatin1Char('/') + QLatin1String(kLib);
            if (QFileInfo::exists(candidate)) libPath = candidate;
        }
        if (libPath.isEmpty()) libPath = QLatin1String(kLib);
    }

    m_lib.setFileName(libPath);
    if (!m_lib.load()) {
        m_lastErr = QStringLiteral("cannot load %1: %2")
                        .arg(libPath, m_lib.errorString());
        if (err) *err = m_lastErr;
        return false;
    }

    m_init   = reinterpret_cast<CallFn>(m_lib.resolve("chronicle_registry_init_registry"));
    m_batch  = reinterpret_cast<CallFn>(m_lib.resolve("chronicle_registry_index_batch"));
    m_getReg = reinterpret_cast<CallFn>(m_lib.resolve("chronicle_registry_get_registry"));
    m_free   = reinterpret_cast<FreeFn>(m_lib.resolve("chronicle_registry_free_string"));
    m_ver    = reinterpret_cast<VerFn> (m_lib.resolve("chronicle_registry_version"));

    if (!m_init || !m_batch || !m_getReg || !m_free || !m_ver) {
        m_lastErr = QStringLiteral("missing symbols in %1").arg(m_lib.fileName());
        if (err) *err = m_lastErr;
        m_lib.unload();
        return false;
    }

    m_loaded = true;
    qDebug() << "FfiClient: loaded" << m_lib.fileName();
    return true;
}

QString FfiClient::ffiVersion() {
    if (!load()) return errJson(m_lastErr);
    char* r = m_ver();
    if (!r) return errJson(QStringLiteral("version() returned null"));
    const QString s = QString::fromUtf8(r);
    m_free(r);
    return s;
}

QString FfiClient::initRegistry(const QString& argsJson) {
    if (!load()) return errJson(m_lastErr);
    return invoke(m_init, argsJson, "init_registry");
}

QString FfiClient::indexBatch(const QString& argsJson) {
    if (!load()) return errJson(m_lastErr);
    return invoke(m_batch, argsJson, "index_batch");
}

QString FfiClient::getRegistry(const QString& argsJson) {
    if (!load()) return errJson(m_lastErr);
    return invoke(m_getReg, argsJson, "get_registry");
}

QString FfiClient::invoke(CallFn fn, const QString& argsJson, const char* name) {
    if (!fn)
        return errJson(QStringLiteral("%1: symbol not resolved")
                           .arg(QString::fromLatin1(name)));
    const QByteArray args = argsJson.toUtf8();
    char* r = fn(args.constData());
    if (!r)
        return errJson(QStringLiteral("%1: returned null")
                           .arg(QString::fromLatin1(name)));
    const QString s = QString::fromUtf8(r);
    m_free(r);
    return s;
}
