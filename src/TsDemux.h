#pragma once

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>

struct Descriptor {
    uint8_t tag = 0;
    QByteArray data;
};

struct PatInfo {
    bool valid = false;
    int tsId = 0;
    int version = 0;
    int nitPid = -1;
    QMap<int, int> programs;  // program_number -> PMT PID
};

struct PmtStream {
    int type = 0;
    int pid = 0;
    QVector<Descriptor> descriptors;
};

struct PmtInfo {
    int programNumber = 0;
    int pid = 0;
    int version = 0;
    int pcrPid = 0;
    QVector<Descriptor> descriptors;
    QVector<PmtStream> streams;
};

struct SdtService {
    int serviceId = 0;
    int type = 0;
    QString provider;
    QString name;
};

struct SdtInfo {
    bool valid = false;
    int tsId = 0;
    int onId = 0;
    int version = 0;
    QVector<SdtService> services;
};

struct PidStats {
    quint64 packets = 0;
    quint64 ccErrors = 0;
    quint64 transportErrors = 0;
    int lastCc = -1;
    bool scrambled = false;
};

class TsDemux : public QObject {
    Q_OBJECT

public:
    explicit TsDemux(QObject* parent = nullptr);

    void reset();
    void feed(const QByteArray& data);

    const PatInfo& pat() const { return pat_; }
    const QMap<int, PmtInfo>& pmts() const { return pmts_; }
    const SdtInfo& sdt() const { return sdt_; }
    const QMap<int, PidStats>& pidStats() const { return pidStats_; }
    quint64 totalPackets() const { return totalPackets_; }
    quint64 syncErrors() const { return syncErrors_; }
    int captionPid() const { return captionPid_; }
    // True when a PMT announces a video or audio elementary stream.
    bool hasAudioVideo() const;

    // Human readable usage of a PID, based on the tables seen so far.
    QString pidUsage(int pid) const;

signals:
    void tablesChanged();
    void captionPidChanged(int pid);
    void captionPes(int pid, const QByteArray& payload, qint64 pts);

private:
    struct SectionBuffer {
        QByteArray data;
        bool active = false;
    };

    struct PesBuffer {
        QByteArray data;
        bool active = false;
    };

    void handlePacket(const uint8_t* p);
    void handleSectionPayload(int pid, const uint8_t* payload, int length, bool unitStart);
    void flushSections(int pid, SectionBuffer& buffer);
    void handleSection(int pid, const QByteArray& section);
    void parsePat(const QByteArray& s);
    void parsePmt(int pid, const QByteArray& s);
    void parseSdt(const QByteArray& s);
    void handlePesPayload(int pid, const uint8_t* payload, int length, bool unitStart);
    void finishPes(int pid, PesBuffer& buffer);
    void selectCaptionPid();

    QByteArray pending_;
    PatInfo pat_;
    QMap<int, PmtInfo> pmts_;
    SdtInfo sdt_;
    QMap<int, PidStats> pidStats_;
    QMap<int, SectionBuffer> sections_;
    QMap<int, PesBuffer> pes_;
    QMap<quint64, QByteArray> lastSections_;  // (pid, table_id, extension) -> raw section, to skip repetitions
    quint64 totalPackets_ = 0;
    quint64 syncErrors_ = 0;
    int captionPid_ = -1;
};

QString streamTypeName(int type);
QString serviceTypeName(int type);
QString decodeDvbString(const QByteArray& bytes);

// Name and decoded value of a descriptor, for display.
QPair<QString, QString> describeDescriptor(const Descriptor& d);
