#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// Structural view of an ARIB caption PES (ARIB STD-B24 / ABNT NBR 15606-1), independent of
// libaribcaption, so the side panel can show the commands sent by the encoder exactly as received.

struct AribLanguageInfo {
    int languageTag = 0;
    int dmf = 0;
    QString iso639;
    int format = 0;
    int tcs = 0;
    int rollupMode = 0;
};

struct AribCommand {
    QString code;         // e.g. "CS", "SDF", "Texto"
    QString parameters;   // e.g. "780;44"
    QString description;  // e.g. "Área de exibição 780×44"
};

struct AribDataUnit {
    int parameter = 0;
    int size = 0;
};

struct AribDataGroup {
    bool valid = false;
    QString error;

    int pesPayloadSize = 0;
    int dataIdentifier = 0;
    int privateStreamId = 0;
    int dataGroupId = 0;
    bool management = false;
    int languageId = 0;  // 1 or 2 for statements
    QChar groupSet;      // 'A' or 'B'
    int size = 0;
    int tmd = 0;

    // CRC-16 of the data group (ARIB STD-B24: x^16+x^12+x^5+1, initial value 0).
    bool crcPresent = false;
    bool crcValid = false;
    quint16 crcSent = 0;
    quint16 crcExpected = 0;
    quint16 crcWithInitFfff = 0;  // common encoder mistake (CRC-16/CCITT-FALSE), used only as a hint

    // Characters outside the ABNT Latin tables, or designations of non-Latin sets.
    QStringList charsetIssues;

    QVector<AribLanguageInfo> languages;  // management only
    QVector<AribDataUnit> units;
    QVector<AribCommand> commands;

    // Writing format values present in this statement (-1 = absent).
    int swf = -1;
    int sdfWidth = -1, sdfHeight = -1;
    int sdpX = -1, sdpY = -1;
    int ssmWidth = -1, ssmHeight = -1;
    int shs = -1, svs = -1;
    int scrMode = -1, scrSpeed = -1;
    bool clearScreen = false;
};

AribDataGroup inspectAribPes(const QByteArray& pesPayload);

QString describeDmf(int dmf);
QString describeRollupMode(int mode);
QString describeTcs(int tcs);
QString describeSwf(int swf);
QString describeDataUnit(int parameter);
