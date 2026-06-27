#ifndef CHRONICLE_ANCHOR_CLIENT_H
#define CHRONICLE_ANCHOR_CLIENT_H

#include <QLibrary>
#include <QObject>
#include <QString>

// Loads chronicle_registry_ffi (built from the repo-root `ffi/`) at runtime
// via QLibrary and exposes its five C entry points as typed methods. All calls
// are JSON-in / JSON-out and blocking; the FFI's tokio runtime handles its own
// async beneath us.
//
// Library path resolution, first to match wins:
//   1. CHRONICLE_REGISTRY_FFI_PATH env var — absolute path; useful for dev /
//      logoscore smoke runs that point at a custom-built .so.
//   2. The bare name "libchronicle_registry_ffi.so" — relies on QLibrary's
//      standard search (LD_LIBRARY_PATH, plugin's own dir if rpath is set).
//
// Loading is lazy: the first anchor method call triggers `ensureLoaded`. If
// the library isn't present, anchor calls fail with a clear error rather than
// crashing chronicle on startup.
class ChronicleAnchorClient : public QObject {
    Q_OBJECT
public:
    explicit ChronicleAnchorClient(QObject* parent = nullptr);
    ~ChronicleAnchorClient() override;

    bool    ensureLoaded(QString* error = nullptr);
    QString lastError() const { return m_lastError; }
    QString libPath() const { return m_lib.fileName(); }

    // Each method takes the FFI's JSON args (object with at least
    // program_id_hex / wallet_path / sequencer_url plus method-specific fields)
    // and returns the FFI's owned heap-allocated JSON string. This class
    // handles `chronicle_registry_free_string` internally.
    QString version();
    QString initRegistry(const QString& argsJson);
    QString indexBatch(const QString& argsJson);
    QString getRegistry(const QString& argsJson);

private:
    using FnCall = char* (*)(const char*);
    using FnFree = void  (*)(char*);
    using FnVer  = char* (*)();

    QString callJson(FnCall fn, const QString& argsJson, const char* fnName);

    QLibrary m_lib;
    bool     m_loaded       = false;
    QString  m_lastError;
    FnCall   m_initRegistry = nullptr;
    FnCall   m_indexBatch   = nullptr;
    FnCall   m_getRegistry  = nullptr;
    FnFree   m_freeString   = nullptr;
    FnVer    m_version      = nullptr;
};

#endif // CHRONICLE_ANCHOR_CLIENT_H
