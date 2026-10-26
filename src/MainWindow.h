#pragma once

#include "AribInspector.h"
#include "CaptionEngine.h"
#include "CaptionValidator.h"
#include "TsDemux.h"
#include "UdpReceiver.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QMap>
#include <QTimer>

class CaptionView;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool initialize(QString* error);
    void setEndpoint(quint16 port, const QString& multicastGroup);
    void startReceiving();
    void showAnalysisTab(int index);

private:
    void buildUi();
    void toggleConnection();
    void stopReceiving();
    void clearAll();

    void onCaptionPes(int pid, const QByteArray& payload, qint64 pts);
    void onCaptionDecoded(const QString& text, qint64 pts, bool clearScreen);
    void appendLog(const QString& message);
    void addCommandsEntry(const AribDataGroup& group);

    void refresh();
    void updateRates();
    void rebuildStreamTree();
    void fillReceptionNode(QTreeWidgetItem* root);
    void fillTablesNodes(QTreeWidgetItem* parent);
    void fillPidNodes(QTreeWidgetItem* root);
    void fillCaptionNode(QTreeWidgetItem* root);
    void rebuildValidationTree();
    void validateLayout();

    UdpReceiver receiver_;
    TsDemux demux_;
    CaptionEngine engine_;
    CaptionValidator validator_;

    CaptionView* view_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QLineEdit* groupEdit_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QCheckBox* overlaysCheck_ = nullptr;
    QTreeWidget* streamTree_ = nullptr;
    QTreeWidget* commandsTree_ = nullptr;
    QTreeWidget* validationTree_ = nullptr;
    QLabel* validationSummary_ = nullptr;
    QLabel* validationStatus_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    int validationTabIndex_ = -1;
    QPlainTextEdit* historyEdit_ = nullptr;
    QPlainTextEdit* logEdit_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* rateLabel_ = nullptr;
    QLabel* captionLabel_ = nullptr;
    QTimer refreshTimer_;

    // Reception rates, sampled on every refresh.
    QElapsedTimer rateClock_;
    quint64 prevPackets_ = 0;
    quint64 prevBytes_ = 0;
    QMap<int, quint64> prevPidPackets_;
    double packetsPerSecond_ = 0;
    double bitsPerSecond_ = 0;
    QMap<int, double> pidBitsPerSecond_;

    // Caption state as announced by the encoder (persists across statements, like a receiver).
    bool hasManagement_ = false;
    AribLanguageInfo management_;
    int swf_ = -1;
    int sdfWidth_ = -1, sdfHeight_ = -1;
    int sdpX_ = -1, sdpY_ = -1;
    int ssmWidth_ = -1, ssmHeight_ = -1;
    int shs_ = -1, svs_ = -1;
    int scrMode_ = -1, scrSpeed_ = -1;
    quint64 managementGroups_ = 0;
    quint64 statementGroups_ = 0;
    quint64 clearScreens_ = 0;
    quint64 inspectErrors_ = 0;
    qint64 lastPts_ = INT64_MIN;
    qint64 prevPts_ = INT64_MIN;
    QDateTime lastCaptionTime_;
    QElapsedTimer lastManagementClock_;
    QElapsedTimer firstStatementClock_;
    QString lastGroupSummary_;
};
