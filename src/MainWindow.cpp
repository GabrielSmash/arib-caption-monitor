#include "MainWindow.h"

#include "CaptionView.h"

#include <QCheckBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kRefreshMs = 500;
constexpr qint64 kSignalTimeoutMs = 2000;
constexpr int kMaxCommandEntries = 300;
constexpr qint64 kNoPts = INT64_MIN;

QString hexPid(int pid) {
    return QString("0x%1 (%2)").arg(pid, 4, 16, QChar('0')).arg(pid);
}

QString formatBitrate(double bps) {
    if (bps >= 1e6)
        return QString("%1 Mbps").arg(bps / 1e6, 0, 'f', 2);
    if (bps >= 1e3)
        return QString("%1 kbps").arg(bps / 1e3, 0, 'f', 1);
    return QString("%1 bps").arg(bps, 0, 'f', 0);
}

QString formatPts(qint64 pts) {
    if (pts == kNoPts)
        return "—";
    return QString("%1 (%2 s)").arg(pts).arg(pts / 90000.0, 0, 'f', 3);
}

QTreeWidgetItem* addItem(QTreeWidgetItem* parent, const QString& name, const QString& value = {}) {
    auto* item = new QTreeWidgetItem(parent, {name, value});
    item->setToolTip(1, value);
    return item;
}

QString itemPath(QTreeWidgetItem* item) {
    QStringList parts;
    for (auto* i = item; i; i = i->parent())
        parts.prepend(i->text(0));
    return parts.join('/');
}

void collectExpanded(QTreeWidgetItem* item, QSet<QString>& out) {
    if (item->isExpanded())
        out.insert(itemPath(item));
    for (int i = 0; i < item->childCount(); ++i)
        collectExpanded(item->child(i), out);
}

void restoreExpanded(QTreeWidgetItem* item, const QSet<QString>& expanded) {
    item->setExpanded(expanded.contains(itemPath(item)));
    for (int i = 0; i < item->childCount(); ++i)
        restoreExpanded(item->child(i), expanded);
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();

    connect(&receiver_, &UdpReceiver::tsData, &demux_, &TsDemux::feed);
    connect(&demux_, &TsDemux::captionPes, this, &MainWindow::onCaptionPes);
    connect(&demux_, &TsDemux::tablesChanged, this, [this]() { validator_.checkPmt(demux_); });
    connect(&demux_, &TsDemux::captionPidChanged, this, [this](int pid) {
        engine_.reset();
        appendLog(pid >= 0 ? QString("PID de legenda selecionado: %1").arg(hexPid(pid))
                           : QString("Nenhum PID de legenda na PMT"));
    });
    connect(&engine_, &CaptionEngine::screenChanged, view_, qOverload<>(&QWidget::update));
    connect(&engine_, &CaptionEngine::captionDecoded, this, &MainWindow::onCaptionDecoded);
    connect(&engine_, &CaptionEngine::logMessage, this, &MainWindow::appendLog);

    connect(&refreshTimer_, &QTimer::timeout, this, &MainWindow::refresh);
    refreshTimer_.start(kRefreshMs);
}

MainWindow::~MainWindow() {
    QSettings settings;
    settings.setValue("port", portSpin_->value());
    settings.setValue("multicast", groupEdit_->text());
    settings.setValue("overlays", overlaysCheck_->isChecked());
    settings.setValue("geometry", saveGeometry());
    settings.setValue("state", saveState());
}

bool MainWindow::initialize(QString* error) {
    return engine_.initialize(error);
}

void MainWindow::buildUi() {
    setWindowTitle("ARIB Caption Monitor — ABNT NBR 15606-1");
    QSettings settings;

    auto* toolbar = addToolBar("Conexão");
    toolbar->setObjectName("connectionToolbar");
    toolbar->setMovable(false);
    toolbar->addWidget(new QLabel(" Porta UDP "));
    portSpin_ = new QSpinBox;
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(settings.value("port", 1526).toInt());
    toolbar->addWidget(portSpin_);
    toolbar->addWidget(new QLabel("  Multicast "));
    groupEdit_ = new QLineEdit(settings.value("multicast").toString());
    groupEdit_->setPlaceholderText("vazio = unicast (ex.: 239.0.0.1)");
    groupEdit_->setMaximumWidth(220);
    toolbar->addWidget(groupEdit_);
    toolbar->addSeparator();
    connectButton_ = new QPushButton("Conectar");
    connect(connectButton_, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    toolbar->addWidget(connectButton_);
    auto* clearButton = new QPushButton("Limpar");
    connect(clearButton, &QPushButton::clicked, this, &MainWindow::clearAll);
    toolbar->addWidget(clearButton);
    toolbar->addSeparator();
    overlaysCheck_ = new QCheckBox("Mostrar posicionamento");
    overlaysCheck_->setChecked(settings.value("overlays", true).toBool());
    toolbar->addWidget(overlaysCheck_);

    view_ = new CaptionView(&engine_);
    view_->setOverlaysVisible(overlaysCheck_->isChecked());
    connect(overlaysCheck_, &QCheckBox::toggled, view_, &CaptionView::setOverlaysVisible);

    historyEdit_ = new QPlainTextEdit;
    historyEdit_->setReadOnly(true);
    historyEdit_->setMaximumBlockCount(2000);
    historyEdit_->setPlaceholderText("Histórico de legendas decodificadas");

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(view_);
    splitter->addWidget(historyEdit_);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    streamTree_ = new QTreeWidget;
    streamTree_->setHeaderLabels({"Item", "Valor"});
    streamTree_->setColumnWidth(0, 210);
    streamTree_->setUniformRowHeights(true);

    commandsTree_ = new QTreeWidget;
    commandsTree_->setHeaderLabels({"Comando", "Detalhe"});
    commandsTree_->setColumnWidth(0, 130);
    commandsTree_->setUniformRowHeights(true);

    logEdit_ = new QPlainTextEdit;
    logEdit_->setReadOnly(true);
    logEdit_->setMaximumBlockCount(1000);

    validationTree_ = new QTreeWidget;
    validationTree_->setHeaderLabels({"Regra", "Status", "Falhas / verificações", "Fonte"});
    validationTree_->setColumnWidth(0, 300);
    validationTree_->setColumnWidth(1, 60);
    validationTree_->setColumnWidth(2, 120);
    validationTree_->setUniformRowHeights(true);
    validationSummary_ = new QLabel;
    validationSummary_->setWordWrap(true);
    auto* validationPage = new QWidget;
    auto* validationLayout = new QVBoxLayout(validationPage);
    validationLayout->setContentsMargins(4, 4, 4, 4);
    validationLayout->addWidget(validationSummary_);
    validationLayout->addWidget(validationTree_);

    tabs_ = new QTabWidget;
    tabs_->addTab(streamTree_, "MPEG-TS");
    validationTabIndex_ = tabs_->addTab(validationPage, "Validação");
    tabs_->addTab(commandsTree_, "Comandos ARIB");
    tabs_->addTab(logEdit_, "Log");

    auto* dock = new QDockWidget("Análise", this);
    dock->setObjectName("analysisDock");
    dock->setWidget(tabs_);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    dock->setMinimumWidth(420);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    stateLabel_ = new QLabel;
    rateLabel_ = new QLabel;
    captionLabel_ = new QLabel;
    statusBar()->addWidget(stateLabel_);
    statusBar()->addWidget(rateLabel_);
    validationStatus_ = new QLabel;
    statusBar()->addPermanentWidget(validationStatus_);
    statusBar()->addPermanentWidget(captionLabel_);

    resize(1500, 820);
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("state").toByteArray());
}

void MainWindow::setEndpoint(quint16 port, const QString& multicastGroup) {
    portSpin_->setValue(port);
    groupEdit_->setText(multicastGroup);
}

void MainWindow::startReceiving() {
    QString error;
    if (!receiver_.start(static_cast<quint16>(portSpin_->value()), groupEdit_->text(), &error)) {
        appendLog(error);
        stateLabel_->setText(error);
        return;
    }
    demux_.reset();
    engine_.reset();
    validator_.reset();
    lastManagementClock_.invalidate();
    firstStatementClock_.invalidate();
    rateClock_.start();
    prevPackets_ = prevBytes_ = 0;
    prevPidPackets_.clear();
    connectButton_->setText("Desconectar");
    portSpin_->setEnabled(false);
    groupEdit_->setEnabled(false);
    appendLog(QString("Escutando UDP na porta %1%2")
                  .arg(receiver_.port())
                  .arg(receiver_.multicastGroup().isEmpty() ? QString(" (unicast)")
                                                            : " multicast " + receiver_.multicastGroup()));
    refresh();
}

void MainWindow::showAnalysisTab(int index) {
    tabs_->setCurrentIndex(index);
}

void MainWindow::stopReceiving() {
    receiver_.stop();
    connectButton_->setText("Conectar");
    portSpin_->setEnabled(true);
    groupEdit_->setEnabled(true);
    appendLog("Recepção parada");
    refresh();
}

void MainWindow::toggleConnection() {
    if (receiver_.isActive())
        stopReceiving();
    else
        startReceiving();
}

void MainWindow::clearAll() {
    engine_.reset();
    historyEdit_->clear();
    commandsTree_->clear();
    managementGroups_ = statementGroups_ = clearScreens_ = inspectErrors_ = 0;
    validator_.reset();
    lastManagementClock_.invalidate();
    firstStatementClock_.invalidate();
    refresh();
}

void MainWindow::onCaptionPes(int pid, const QByteArray& payload, qint64 pts) {
    Q_UNUSED(pid);
    const AribDataGroup group = inspectAribPes(payload);
    if (!group.valid) {
        ++inspectErrors_;
        appendLog(QString("Data group inválido: %1").arg(group.error));
    }

    if (group.management) {
        ++managementGroups_;
        for (const auto& lang : group.languages) {
            if (lang.languageTag == 0) {
                management_ = lang;
                hasManagement_ = true;
            }
        }
    } else {
        ++statementGroups_;
        if (!firstStatementClock_.isValid())
            firstStatementClock_.start();
        if (group.clearScreen) {
            ++clearScreens_;
            // CS resets the writing format: without a new SDF/SDP the display area is the whole plane again.
            sdfWidth_ = sdfHeight_ = sdpX_ = sdpY_ = -1;
        }
        if (group.swf >= 0)
            swf_ = group.swf;
        if (group.sdfWidth >= 0) {
            sdfWidth_ = group.sdfWidth;
            sdfHeight_ = group.sdfHeight;
        }
        if (group.sdpX >= 0) {
            sdpX_ = group.sdpX;
            sdpY_ = group.sdpY;
        }
        if (group.ssmWidth >= 0) {
            ssmWidth_ = group.ssmWidth;
            ssmHeight_ = group.ssmHeight;
        }
        if (group.shs >= 0)
            shs_ = group.shs;
        if (group.svs >= 0)
            svs_ = group.svs;
        if (group.scrMode >= 0) {
            scrMode_ = group.scrMode;
            scrSpeed_ = group.scrSpeed;
        }
        prevPts_ = lastPts_;
        lastPts_ = pts;
        lastCaptionTime_ = QDateTime::currentDateTime();

        view_->setDisplayArea(sdfWidth_ >= 0 && sdpX_ >= 0 ? QRect(sdpX_, sdpY_, sdfWidth_, sdfHeight_) : QRect());
    }

    if (group.management)
        lastManagementClock_.start();
    validator_.checkDataGroup(group);
    addCommandsEntry(group);
    if (engine_.processPes(payload, pts))
        validateLayout();
}

void MainWindow::validateLayout() {
    QVector<QRect> regions;
    QStringList texts;
    for (const auto& region : engine_.lastCaptionRegions()) {
        regions << region.rect;
        texts << region.text;
    }
    const QRect area = sdfWidth_ >= 0 && sdpX_ >= 0 ? QRect(sdpX_, sdpY_, sdfWidth_, sdfHeight_)
                                                   : QRect(QPoint(0, 0), engine_.planeSize());
    validator_.checkLayout(engine_.planeSize(), area, regions, texts);
}

void MainWindow::onCaptionDecoded(const QString& text, qint64 pts, bool clearScreen) {
    Q_UNUSED(pts);
    if (text.isEmpty())
        return;
    QString line = text;
    line.replace('\n', " ⏎ ");
    historyEdit_->appendPlainText(QString("[%1]%2 %3")
                                      .arg(QTime::currentTime().toString("HH:mm:ss.zzz"))
                                      .arg(clearScreen ? " [CS]" : "")
                                      .arg(line));
}

void MainWindow::appendLog(const QString& message) {
    logEdit_->appendPlainText(QString("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss.zzz"), message));
}

void MainWindow::addCommandsEntry(const AribDataGroup& group) {
    QString title;
    if (group.management) {
        QStringList langs;
        for (const auto& lang : group.languages)
            langs << lang.iso639;
        title = QString("Gerenciamento %1 — %2 — %3 B").arg(group.groupSet).arg(langs.join(", ")).arg(group.size);
    } else {
        title = QString("Statement %1, idioma %2 — %3 B").arg(group.groupSet).arg(group.languageId).arg(group.size);
    }
    lastGroupSummary_ = title;

    auto* top = new QTreeWidgetItem({QTime::currentTime().toString("HH:mm:ss.zzz"), title});
    if (!group.valid)
        top->setForeground(1, Qt::red);

    addItem(top, "data_group_id", QString("0x%1").arg(group.dataGroupId, 2, 16, QChar('0')));
    addItem(top, "TMD", QString::number(group.tmd));
    for (const auto& lang : group.languages) {
        auto* l = addItem(top, "Idioma", QString("%1 (tag %2)").arg(lang.iso639).arg(lang.languageTag));
        addItem(l, "DMF", describeDmf(lang.dmf));
        addItem(l, "Formato", QString("%1 → SWF %2 (%3)").arg(lang.format).arg(lang.format - 1).arg(describeSwf(lang.format - 1)));
        addItem(l, "TCS", describeTcs(lang.tcs));
        addItem(l, "rollup_mode", describeRollupMode(lang.rollupMode));
    }
    for (const auto& unit : group.units)
        addItem(top, "Data unit", QString("%1 — %2 B").arg(describeDataUnit(unit.parameter)).arg(unit.size));
    for (const auto& cmd : group.commands) {
        const QString detail = cmd.parameters.isEmpty() ? cmd.description
                                                        : QString("%1  —  %2").arg(cmd.parameters, cmd.description);
        auto* item = addItem(top, cmd.code, detail);
        if (cmd.code == "CS" || cmd.code == "SCR")
            item->setForeground(0, QColor(220, 120, 0));
    }
    if (!group.error.isEmpty())
        addItem(top, "Erro", group.error);

    if (auto* previous = commandsTree_->topLevelItem(0))
        previous->setExpanded(false);
    commandsTree_->insertTopLevelItem(0, top);
    top->setExpanded(!group.management);
    while (commandsTree_->topLevelItemCount() > kMaxCommandEntries)
        delete commandsTree_->takeTopLevelItem(commandsTree_->topLevelItemCount() - 1);
}

void MainWindow::refresh() {
    updateRates();

    const qint64 since = receiver_.msSinceLastDatagram();
    QString state;
    QColor color;
    if (!receiver_.isActive()) {
        state = "Desconectado";
        color = QColor(120, 120, 120);
    } else if (since < 0) {
        state = QString("Aguardando pacotes na porta %1…").arg(receiver_.port());
        color = QColor(230, 160, 30);
    } else if (since > kSignalTimeoutMs) {
        state = QString("Sem sinal há %1 s").arg(since / 1000.0, 0, 'f', 1);
        color = QColor(220, 50, 50);
    } else {
        state = "Recebendo";
        color = QColor(40, 200, 80);
    }
    view_->setSignalState(state, color);
    stateLabel_->setText(QString("<b style='color:%1'>●</b> %2").arg(color.name(), state));
    rateLabel_->setText(QString("  %1 pkt/s · %2 · %3 datagramas")
                            .arg(packetsPerSecond_, 0, 'f', 0)
                            .arg(formatBitrate(bitsPerSecond_))
                            .arg(receiver_.datagrams()));
    const int pid = demux_.captionPid();
    captionLabel_->setText(QString("Legenda PID %1 · %2 legendas · última %3")
                               .arg(pid >= 0 ? hexPid(pid) : QString("—"))
                               .arg(engine_.captionsDecoded())
                               .arg(lastCaptionTime_.isValid() ? lastCaptionTime_.toString("HH:mm:ss") : "—"));

    if (receiver_.isActive()) {
        validator_.checkManagementInterval(lastManagementClock_.isValid() ? lastManagementClock_.elapsed() : -1,
                                           firstStatementClock_.isValid() ? firstStatementClock_.elapsed() : -1);
    }

    rebuildStreamTree();
    rebuildValidationTree();
}

void MainWindow::rebuildValidationTree() {
    using Severity = CaptionValidator::Severity;
    const int errors = validator_.failingRules(Severity::Error);
    const int warnings = validator_.failingRules(Severity::Warning);
    const int infos = validator_.failingRules(Severity::Info);
    const int passing = validator_.passingRules();

    validationSummary_->setText(
        QString("<b style='color:#d03030'>%1 erro(s)</b> · <b style='color:#c08000'>%2 aviso(s)</b> · "
                "%3 informativo(s) · <span style='color:#2a9d4a'>%4 regra(s) OK</span><br>"
                "<small>Sem o texto da ABNT NBR 15606-1, regras da operação japonesa (ARIB TR-B14) aparecem como "
                "aviso. A coluna Fonte indica a origem de cada regra.</small>")
            .arg(errors)
            .arg(warnings)
            .arg(infos)
            .arg(passing));
    tabs_->setTabText(validationTabIndex_, errors + warnings > 0 ? QString("Validação (%1)").arg(errors + warnings)
                                                                 : QString("Validação"));
    validationStatus_->setText(
        errors > 0     ? QString("<b style='color:#d03030'>Validação: %1 erro(s), %2 aviso(s)</b>").arg(errors).arg(warnings)
        : warnings > 0 ? QString("<b style='color:#c08000'>Validação: %1 aviso(s)</b>").arg(warnings)
                       : QString("<span style='color:#2a9d4a'>Validação OK</span>"));

    QSet<QString> expanded;
    for (int i = 0; i < validationTree_->topLevelItemCount(); ++i)
        collectExpanded(validationTree_->topLevelItem(i), expanded);
    const int scroll = validationTree_->verticalScrollBar()->value();
    validationTree_->setUpdatesEnabled(false);
    validationTree_->clear();

    // Failing rules first (errors, warnings, info), then passing, then not yet checked.
    auto rank = [](const CaptionValidator::Rule& rule) {
        if (rule.failures > 0)
            return static_cast<int>(rule.severity);
        return rule.checks > 0 ? 3 : 4;
    };
    QVector<const CaptionValidator::Rule*> ordered;
    for (const auto& rule : validator_.rules())
        ordered << &rule;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](const auto* a, const auto* b) { return rank(*a) < rank(*b); });

    for (const auto* rule : ordered) {
        QString status;
        QColor color;
        if (rule->failures > 0) {
            switch (rule->severity) {
                case Severity::Error: status = "ERRO"; color = QColor(208, 48, 48); break;
                case Severity::Warning: status = "AVISO"; color = QColor(192, 128, 0); break;
                case Severity::Info: status = "INFO"; color = QColor(60, 110, 200); break;
            }
        } else if (rule->checks > 0) {
            status = "OK";
            color = QColor(42, 157, 74);
        } else {
            status = "—";
            color = QColor(140, 140, 140);
        }

        auto* item = new QTreeWidgetItem(validationTree_, {rule->title, status,
                                                            QString("%1 / %2").arg(rule->failures).arg(rule->checks),
                                                            rule->source});
        item->setForeground(1, color);
        QFont bold = item->font(1);
        bold.setBold(true);
        item->setFont(1, bold);
        item->setToolTip(0, rule->title);
        item->setToolTip(3, rule->source);
        for (const auto& occurrence : rule->recent) {
            auto* child = new QTreeWidgetItem(item, {occurrence.time.toString("HH:mm:ss.zzz"), occurrence.detail});
            child->setToolTip(1, occurrence.detail);
        }
    }

    for (int i = 0; i < validationTree_->topLevelItemCount(); ++i)
        restoreExpanded(validationTree_->topLevelItem(i), expanded);
    validationTree_->setUpdatesEnabled(true);
    validationTree_->verticalScrollBar()->setValue(scroll);
}

void MainWindow::updateRates() {
    if (!rateClock_.isValid())
        return;
    const double seconds = rateClock_.restart() / 1000.0;
    if (seconds <= 0)
        return;

    const quint64 packets = demux_.totalPackets();
    const quint64 bytes = receiver_.bytes();
    packetsPerSecond_ = (packets - prevPackets_) / seconds;
    bitsPerSecond_ = (bytes - prevBytes_) * 8 / seconds;
    prevPackets_ = packets;
    prevBytes_ = bytes;

    const auto& stats = demux_.pidStats();
    for (auto it = stats.begin(); it != stats.end(); ++it) {
        const quint64 previous = prevPidPackets_.value(it.key(), 0);
        pidBitsPerSecond_[it.key()] = (it.value().packets - previous) * 188 * 8 / seconds;
        prevPidPackets_[it.key()] = it.value().packets;
    }
}

void MainWindow::rebuildStreamTree() {
    QSet<QString> expanded;
    const bool firstBuild = streamTree_->topLevelItemCount() == 0;
    for (int i = 0; i < streamTree_->topLevelItemCount(); ++i)
        collectExpanded(streamTree_->topLevelItem(i), expanded);
    const int scroll = streamTree_->verticalScrollBar()->value();

    streamTree_->setUpdatesEnabled(false);
    streamTree_->clear();

    fillReceptionNode(new QTreeWidgetItem(streamTree_, {"Recepção"}));
    fillTablesNodes(streamTree_->invisibleRootItem());
    fillPidNodes(new QTreeWidgetItem(streamTree_, {"PIDs"}));
    fillCaptionNode(new QTreeWidgetItem(streamTree_, {"Legenda ARIB (ABNT NBR 15606-1)"}));

    for (int i = 0; i < streamTree_->topLevelItemCount(); ++i) {
        auto* item = streamTree_->topLevelItem(i);
        if (firstBuild)
            item->setExpanded(true);
        else
            restoreExpanded(item, expanded);
    }
    streamTree_->setUpdatesEnabled(true);
    streamTree_->verticalScrollBar()->setValue(scroll);
}

void MainWindow::fillReceptionNode(QTreeWidgetItem* root) {
    addItem(root, "Estado", receiver_.isActive() ? "Escutando" : "Desconectado");
    addItem(root, "Porta UDP", receiver_.isActive() ? QString::number(receiver_.port()) : "—");
    addItem(root, "Multicast", receiver_.multicastGroup().isEmpty() ? "não (unicast)" : receiver_.multicastGroup());
    addItem(root, "Origem", receiver_.lastSender().isEmpty() ? "—" : receiver_.lastSender());
    const qint64 since = receiver_.msSinceLastDatagram();
    addItem(root, "Último datagrama", since < 0 ? "—" : QString("há %1 ms").arg(since));
    addItem(root, "Datagramas", QString::number(receiver_.datagrams()));
    addItem(root, "Encapsulamento", receiver_.rtpDatagrams() > 0 ? "RTP" : "UDP puro");
    addItem(root, "Pacotes TS", QString::number(demux_.totalPackets()));
    addItem(root, "Taxa", QString("%1 pkt/s").arg(packetsPerSecond_, 0, 'f', 0));
    addItem(root, "Bitrate", formatBitrate(bitsPerSecond_));
    addItem(root, "Erros de sincronismo", QString::number(demux_.syncErrors()));
}

void MainWindow::fillTablesNodes(QTreeWidgetItem* parent) {
    const PatInfo& pat = demux_.pat();
    auto* patItem = addItem(parent, "PAT", pat.valid ? QString("PID 0x0000, versão %1").arg(pat.version) : "não recebida");
    if (pat.valid) {
        addItem(patItem, "transport_stream_id", QString("0x%1 (%2)").arg(pat.tsId, 4, 16, QChar('0')).arg(pat.tsId));
        if (pat.nitPid >= 0)
            addItem(patItem, "NIT", hexPid(pat.nitPid));
        for (auto it = pat.programs.begin(); it != pat.programs.end(); ++it)
            addItem(patItem, QString("Programa %1").arg(it.key()), "PMT " + hexPid(it.value()));
    }

    for (const auto& pmt : demux_.pmts()) {
        auto* pmtItem = addItem(parent, QString("PMT programa %1").arg(pmt.programNumber),
                                QString("PID %1, versão %2").arg(hexPid(pmt.pid)).arg(pmt.version));
        addItem(pmtItem, "PCR PID", pmt.pcrPid == 0x1FFF ? "nenhum" : hexPid(pmt.pcrPid));
        for (const auto& d : pmt.descriptors) {
            const auto desc = describeDescriptor(d);
            addItem(pmtItem, desc.first, desc.second);
        }
        for (const auto& stream : pmt.streams) {
            QString label = QString("0x%1 %2").arg(stream.type, 2, 16, QChar('0')).arg(streamTypeName(stream.type));
            if (stream.pid == demux_.captionPid())
                label += " — LEGENDA";
            auto* streamItem = addItem(pmtItem, "ES " + hexPid(stream.pid), label);
            for (const auto& d : stream.descriptors) {
                const auto desc = describeDescriptor(d);
                addItem(streamItem, desc.first, desc.second);
            }
        }
    }

    const SdtInfo& sdt = demux_.sdt();
    auto* sdtItem = addItem(parent, "SDT", sdt.valid ? QString("PID 0x0011, versão %1").arg(sdt.version) : "não recebida");
    if (sdt.valid) {
        addItem(sdtItem, "original_network_id", QString("0x%1 (%2)").arg(sdt.onId, 4, 16, QChar('0')).arg(sdt.onId));
        addItem(sdtItem, "transport_stream_id", QString("0x%1 (%2)").arg(sdt.tsId, 4, 16, QChar('0')).arg(sdt.tsId));
        for (const auto& service : sdt.services) {
            auto* s = addItem(sdtItem, QString("Serviço 0x%1").arg(service.serviceId, 4, 16, QChar('0')), service.name);
            addItem(s, "Provedor", service.provider);
            addItem(s, "Tipo", serviceTypeName(service.type));
        }
    }
}

void MainWindow::fillPidNodes(QTreeWidgetItem* root) {
    const auto& stats = demux_.pidStats();
    root->setText(1, QString("%1 PIDs").arg(stats.size()));
    const double total = qMax<quint64>(1, demux_.totalPackets());
    for (auto it = stats.begin(); it != stats.end(); ++it) {
        const PidStats& s = it.value();
        auto* item = addItem(root, hexPid(it.key()), demux_.pidUsage(it.key()));
        addItem(item, "Pacotes", QString("%1 (%2%)").arg(s.packets).arg(100.0 * s.packets / total, 0, 'f', 1));
        addItem(item, "Bitrate", formatBitrate(pidBitsPerSecond_.value(it.key())));
        addItem(item, "Erros de continuidade", QString::number(s.ccErrors));
        addItem(item, "Erros de transporte", QString::number(s.transportErrors));
        addItem(item, "Embaralhado", s.scrambled ? "sim" : "não");
        if (s.ccErrors > 0 || s.transportErrors > 0)
            item->setForeground(1, Qt::red);
    }
}

void MainWindow::fillCaptionNode(QTreeWidgetItem* root) {
    const int pid = demux_.captionPid();
    root->setText(1, pid >= 0 ? hexPid(pid) : "PID não identificado");

    auto* mgmt = addItem(root, "Gerenciamento", hasManagement_ ? QString("%1 grupos").arg(managementGroups_) : "não recebido");
    if (hasManagement_) {
        addItem(mgmt, "Idioma", management_.iso639);
        addItem(mgmt, "DMF", describeDmf(management_.dmf));
        addItem(mgmt, "Formato", QString("%1 (%2)").arg(management_.format).arg(describeSwf(management_.format - 1)));
        addItem(mgmt, "TCS", describeTcs(management_.tcs));
        addItem(mgmt, "rollup_mode", describeRollupMode(management_.rollupMode));
    }

    auto* format = addItem(root, "Formato de exibição");
    auto orUnknown = [](int v, const QString& text) { return v >= 0 ? text : QString("—"); };
    addItem(format, "Plano", QString("%1×%2").arg(engine_.planeSize().width()).arg(engine_.planeSize().height()));
    addItem(format, "SWF", orUnknown(swf_, QString("%1 (%2)").arg(swf_).arg(describeSwf(swf_))));
    addItem(format, "SDP (posição da área)", orUnknown(sdpX_, QString("x=%1 y=%2").arg(sdpX_).arg(sdpY_)));
    addItem(format, "SDF (tamanho da área)", orUnknown(sdfWidth_, QString("%1×%2").arg(sdfWidth_).arg(sdfHeight_)));
    addItem(format, "SSM (caractere)", orUnknown(ssmWidth_, QString("%1×%2 dots").arg(ssmWidth_).arg(ssmHeight_)));
    addItem(format, "SHS / SVS (espaçamento)",
            QString("%1 / %2").arg(orUnknown(shs_, QString::number(shs_)), orUnknown(svs_, QString::number(svs_))));
    addItem(format, "SCR (scroll)", scrMode_ < 0 ? "não utilizado" : QString("modo %1, velocidade %2").arg(scrMode_).arg(scrSpeed_));

    auto* screen = addItem(root, "Tela atual", QString("%1 região(ões)").arg(engine_.regions().size()));
    for (const auto& region : engine_.regions()) {
        const QRect& r = region.rect;
        addItem(screen, QString("x=%1 y=%2 %3×%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()), region.text);
    }

    auto* stats = addItem(root, "Estatísticas");
    addItem(stats, "Statements recebidos", QString::number(statementGroups_));
    addItem(stats, "Legendas decodificadas", QString::number(engine_.captionsDecoded()));
    addItem(stats, "Limpezas de tela (CS)", QString::number(clearScreens_));
    addItem(stats, "Erros de decodificação", QString::number(engine_.decodeErrors() + inspectErrors_));
    addItem(stats, "Último data group", lastGroupSummary_.isEmpty() ? "—" : lastGroupSummary_);
    addItem(stats, "PTS último", formatPts(lastPts_));
    if (lastPts_ != kNoPts && prevPts_ != kNoPts)
        addItem(stats, "Δ PTS", QString("%1 ms").arg((lastPts_ - prevPts_) / 90.0, 0, 'f', 0));
}
