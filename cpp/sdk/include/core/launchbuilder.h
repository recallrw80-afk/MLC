#ifndef MLC_LAUNCHBUILDER_H
#define MLC_LAUNCHBUILDER_H

#include <QObject>
#include <QMap>
#include <QStringList>
#include <nlohmann/json.hpp>
#include "core/types.h"
#include "core/mlccore_export.h"

using json = nlohmann::json;

/**
 * Builds the JVM and game arguments for Minecraft launching.
 * Mirrors the argument construction in ModLaunch.McLaunchArgumentMain,
 * McLaunchArgumentsJVM, McLaunchArgumentsGame, and McLaunchArgumentsReplace.
 */
class MLCCORE_EXPORT LaunchBuilder : public QObject
{
    Q_OBJECT

public:
    static LaunchBuilder& instance();

    /// Set the version JSON to use for argument building
    void setVersionJson(const json &versionJson);

    /// Set launch options
    void setLaunchOptions(const McLaunchOptions &options);

    /// Build the complete launch argument string
    /// Sets m_jvmArgs and m_gameArgs
    bool build(const McVersion &version,
               const JavaEntry &java,
               const LoginResult &login);

    /// Get built JVM arguments
    QStringList jvmArgs() const { return m_jvmArgs; }

    /// Get built game arguments
    QStringList gameArgs() const { return m_gameArgs; }

    /// Get the main class
    QString mainClass() const { return m_mainClass; }

    /// Get the complete command line (for display)
    QString commandLine() const;

    /// Build the replacement map (${key} -> value)
    QMap<QString, QString> buildReplacements(const McVersion &version,
                                              const JavaEntry &java,
                                              const LoginResult &login);

    /// Apply replacements to arguments
    QStringList applyReplacements(const QStringList &args,
                                   const QMap<QString, QString> &repl) const;

    // ---- Static rule checking ----

    /// Check if a rules array allows the current platform
    static bool checkRules(const json &rules);

    /// Check if a feature set is enabled
    static bool checkFeatures(const json &features);

private:
    /// Build JVM arguments from version JSON
    QStringList buildJvmArgs(const McVersion &version, const JavaEntry &java);

    /// Build game arguments from version JSON
    QStringList buildGameArgs(const McVersion &version, const LoginResult &login);

    json m_versionJson;
    McLaunchOptions m_options;
    QStringList m_jvmArgs;
    QStringList m_gameArgs;
    QString m_mainClass;
    QStringList m_commandLine; ///< Fully substituted JVM+main+game args, cached by build()
};

#endif // MLC_LAUNCHBUILDER_H
