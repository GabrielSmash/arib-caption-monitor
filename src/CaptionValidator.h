#pragma once

#include "AribInspector.h"

#include <QDateTime>
#include <QHash>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

class TsDemux;

// Automatic conformance checks for the caption stream. Every rule carries the source it comes from,
// because the ABNT NBR 15606-1 text is not available: rules based on Japanese ARIB operation (TR-B14)
// are warnings, not errors, since they may not apply to Brazil.
class CaptionValidator {
public:
    enum class Severity { Error, Warning, Info };

    struct Occurrence {
        QDateTime time;
        QString detail;
    };

    struct Rule {
        QString id;
        Severity severity = Severity::Error;
        QString title;
        QString source;
        quint64 checks = 0;
        quint64 failures = 0;
        QVector<Occurrence> recent;  // newest first
    };

    CaptionValidator();

    void reset();
    void checkPmt(const TsDemux& demux);
    void checkDataGroup(const AribDataGroup& group);
    // Layout of the caption just decoded, in caption plane coordinates. displayArea is null when unknown.
    void checkLayout(const QSize& plane, const QRect& displayArea, const QVector<QRect>& regions,
                     const QStringList& texts);
    // msSinceManagement = -1 when no management data was received yet.
    void checkManagementInterval(qint64 msSinceManagement, qint64 msSinceFirstStatement);

    const QVector<Rule>& rules() const { return rules_; }
    int failingRules(Severity severity) const;
    int passingRules() const;

private:
    void addRule(const QString& id, Severity severity, const QString& title, const QString& source);
    void record(const QString& id, bool ok, const QString& detail = {});

    QVector<Rule> rules_;
    QHash<QString, int> index_;
    bool managementLate_ = false;
};
